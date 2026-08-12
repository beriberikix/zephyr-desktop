/*
 * zephyr-desktop — palette, shared styles, and the bevel renderer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include "theme.h"
#include "titlebar.h"

lv_style_t zd_style_face;

/* --- desktop background pattern ------------------------------------------- */

#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3))
#define LO(c)           ((uint8_t)((c) & 0xFF))
#define HI(c)           ((uint8_t)((c) >> 8))

#define TEAL   RGB565(0x00, 0x80, 0x80)
#define TEAL_D RGB565(0x00, 0x60, 0x60)

/* An 8x8 tile of 2x2 checks. At 480x272 the eye reads it as texture rather than
 * as a grid, which is the point -- a flat fill looks like a bug, not a desktop.
 */
#define A LO(TEAL), HI(TEAL)
#define B LO(TEAL_D), HI(TEAL_D)

static const uint8_t desktop_tile[8 * 8 * 2] = {
	A, A, B, B, A, A, B, B,
	A, A, B, B, A, A, B, B,
	B, B, A, A, B, B, A, A,
	B, B, A, A, B, B, A, A,
	A, A, B, B, A, A, B, B,
	A, A, B, B, A, A, B, B,
	B, B, A, A, B, B, A, A,
	B, B, A, A, B, B, A, A,
};

#undef A
#undef B

const lv_image_dsc_t zd_desktop_pattern = {
	.header = {
		.magic = LV_IMAGE_HEADER_MAGIC,
		.cf = LV_COLOR_FORMAT_RGB565,
		.w = 8,
		.h = 8,
		.stride = 8 * 2,
	},
	.data_size = sizeof(desktop_tile),
	.data = desktop_tile,
};

/* --- bevels ---------------------------------------------------------------- */

/* Draw a one-pixel ring: top and left in @p tl, bottom and right in @p br. */
static void draw_ring(lv_layer_t *layer, const lv_area_t *a, uint32_t tl, uint32_t br)
{
	lv_draw_rect_dsc_t dsc;
	lv_area_t r;

	if (a->x2 <= a->x1 || a->y2 <= a->y1) {
		return;
	}

	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_opa = LV_OPA_COVER;
	dsc.border_width = 0;
	dsc.radius = 0;

	dsc.bg_color = lv_color_hex(tl);
	r.x1 = a->x1; r.y1 = a->y1; r.x2 = a->x2; r.y2 = a->y1; /* top */
	lv_draw_rect(layer, &dsc, &r);
	r.x1 = a->x1; r.y1 = a->y1; r.x2 = a->x1; r.y2 = a->y2; /* left */
	lv_draw_rect(layer, &dsc, &r);

	dsc.bg_color = lv_color_hex(br);
	r.x1 = a->x1; r.y1 = a->y2; r.x2 = a->x2; r.y2 = a->y2; /* bottom */
	lv_draw_rect(layer, &dsc, &r);
	r.x1 = a->x2; r.y1 = a->y1; r.x2 = a->x2; r.y2 = a->y2; /* right */
	lv_draw_rect(layer, &dsc, &r);
}

void zd_bevel_draw(lv_layer_t *layer, const lv_area_t *area, zd_bevel_t kind)
{
	lv_area_t inner;

	inner.x1 = area->x1 + 1;
	inner.y1 = area->y1 + 1;
	inner.x2 = area->x2 - 1;
	inner.y2 = area->y2 - 1;

	if (kind == ZD_BEVEL_OUT) {
		draw_ring(layer, area, ZD_C_LIGHT, ZD_C_DARK);
		draw_ring(layer, &inner, ZD_C_FACE_LIGHT, ZD_C_SHADOW);
	} else {
		draw_ring(layer, area, ZD_C_SHADOW, ZD_C_LIGHT);
		draw_ring(layer, &inner, ZD_C_DARK, ZD_C_FACE_LIGHT);
	}
}

static void bevel_draw_cb(lv_event_t *e)
{
	zd_bevel_t kind = (zd_bevel_t)(uintptr_t)lv_event_get_user_data(e);
	lv_obj_t *obj = lv_event_get_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t outer;

	lv_obj_get_coords(obj, &outer);

	if (kind == ZD_BEVEL_BUTTON) {
		/* Reading the state here rather than swapping the callback keeps
		 * press feedback a pure function of LVGL state -- nothing to keep
		 * in sync, and nothing to leak if the object dies mid-press.
		 *
		 * CHECKED reads the same as PRESSED, which is how Win95 drew a
		 * toggle that is on: held down. The taskbar's keyboard button
		 * is the first user. Note that nothing fires an event when
		 * CHECKED is set by code, so a caller that toggles it must
		 * invalidate; see zd_taskbar_set_osk_active().
		 */
		kind = (lv_obj_get_state(obj) & (LV_STATE_PRESSED | LV_STATE_CHECKED))
			       ? ZD_BEVEL_IN
			       : ZD_BEVEL_OUT;
	}

	zd_bevel_draw(layer, &outer, kind);
}

/* LVGL only invalidates on a state change when some *style* property depends on
 * that state. A bevel drawn in DRAW_POST is invisible to that check, so a press
 * would flip the state and never repaint. Ask for the redraw explicitly.
 */
static void bevel_state_cb(lv_event_t *e)
{
	lv_obj_invalidate(lv_event_get_target_obj(e));
}

void zd_bevel_attach(lv_obj_t *obj, zd_bevel_t kind)
{
	lv_obj_add_event_cb(obj, bevel_draw_cb, LV_EVENT_DRAW_POST,
			    (void *)(uintptr_t)kind);

	if (kind == ZD_BEVEL_BUTTON) {
		lv_obj_add_event_cb(obj, bevel_state_cb, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(obj, bevel_state_cb, LV_EVENT_RELEASED, NULL);
		lv_obj_add_event_cb(obj, bevel_state_cb, LV_EVENT_PRESS_LOST, NULL);
	}
}

/* --- styles ---------------------------------------------------------------- */

void zd_theme_init(void)
{
	static bool done;

	if (done) {
		return;
	}
	done = true;

	lv_style_init(&zd_style_face);
	lv_style_set_bg_color(&zd_style_face, lv_color_hex(ZD_C_FACE));
	lv_style_set_bg_opa(&zd_style_face, LV_OPA_COVER);
	lv_style_set_border_width(&zd_style_face, 0);
	lv_style_set_outline_width(&zd_style_face, 0);
	lv_style_set_radius(&zd_style_face, 0);
	lv_style_set_pad_all(&zd_style_face, 0);
	lv_style_set_text_color(&zd_style_face, lv_color_hex(ZD_C_TEXT));
	lv_style_set_text_font(&zd_style_face, &lv_font_montserrat_12);

	zd_titlebar_styles_init();
}
