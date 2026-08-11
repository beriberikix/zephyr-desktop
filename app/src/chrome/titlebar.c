/*
 * zephyr-desktop — titlebar styles and the close glyph.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "titlebar.h"
#include "theme.h"

lv_style_t zd_style_title_active;
lv_style_t zd_style_title_inactive;

static void title_style(lv_style_t *style, uint32_t bg)
{
	lv_style_init(style);
	lv_style_set_bg_color(style, lv_color_hex(bg));
	lv_style_set_bg_opa(style, LV_OPA_COVER);
	lv_style_set_border_width(style, 0);
	lv_style_set_outline_width(style, 0);
	lv_style_set_radius(style, 0);
	lv_style_set_pad_all(style, 0);
	lv_style_set_text_color(style, lv_color_hex(ZD_C_TITLE_TEXT));
	lv_style_set_text_font(style, &lv_font_montserrat_12);
}

void zd_titlebar_styles_init(void)
{
	title_style(&zd_style_title_active, ZD_C_TITLE_ACTIVE);
	title_style(&zd_style_title_inactive, ZD_C_TITLE_INACTIVE);
}

void zd_titlebar_set_active(lv_obj_t *titlebar, bool active)
{
	lv_obj_remove_style(titlebar, &zd_style_title_active, LV_PART_MAIN);
	lv_obj_remove_style(titlebar, &zd_style_title_inactive, LV_PART_MAIN);
	lv_obj_add_style(titlebar, active ? &zd_style_title_active : &zd_style_title_inactive,
			 LV_PART_MAIN);
	lv_obj_invalidate(titlebar);
}

/* --- close glyph ----------------------------------------------------------- */

#define GLYPH_SPAN 7 /* the X is 7x7, like the real thing */

static void put_px(lv_layer_t *layer, lv_draw_rect_dsc_t *dsc, int32_t x, int32_t y)
{
	lv_area_t a = { .x1 = x, .y1 = y, .x2 = x, .y2 = y };

	lv_draw_rect(layer, dsc, &a);
}

static void close_glyph_cb(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_target_obj(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_draw_rect_dsc_t dsc;
	lv_area_t coords;
	int32_t ox;
	int32_t oy;

	lv_obj_get_coords(obj, &coords);

	/* Centre the glyph, then nudge it down-right by one while pressed so the
	 * label appears to move into the sunken bevel, as Win95 does.
	 */
	ox = coords.x1 + (lv_area_get_width(&coords) - GLYPH_SPAN) / 2;
	oy = coords.y1 + (lv_area_get_height(&coords) - GLYPH_SPAN) / 2;
	if (lv_obj_get_state(obj) & LV_STATE_PRESSED) {
		ox += 1;
		oy += 1;
	}

	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_opa = LV_OPA_COVER;
	dsc.bg_color = lv_color_hex(ZD_C_TEXT);
	dsc.border_width = 0;
	dsc.radius = 0;

	for (int32_t i = 0; i < GLYPH_SPAN; i++) {
		put_px(layer, &dsc, ox + i, oy + i);
		put_px(layer, &dsc, ox + (GLYPH_SPAN - 1 - i), oy + i);
	}
}

void zd_close_glyph_attach(lv_obj_t *obj)
{
	/* Registered after the bevel's own DRAW_POST handler, so it paints over
	 * the bevel rather than under them.
	 */
	lv_obj_add_event_cb(obj, close_glyph_cb, LV_EVENT_DRAW_POST, NULL);
}
