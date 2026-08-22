/*
 * zephyr-desktop — the assistant balloon.
 *
 * One lv_obj_t draws the icon and the panel in a single callback, plus one
 * child label for the text. The label is a real lv_label rather than a
 * lv_draw_label in the callback because it has to wrap, and wrapping is the one
 * thing worth having an object for -- LVGL already solves it, and solving it
 * again inside a draw callback would mean measuring text by hand.
 *
 * That child is safe under this project's never-destroy-during-dispatch rule
 * for the same reason chrome/cellgrid.c is: nothing here is ever destroyed in
 * response to an event. A zapp answering an event by changing the balloon's
 * text calls lv_label_set_text, which rewrites a buffer and invalidates a
 * rectangle. No object is created, none is deleted, and there is no reap.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zd/zapp_abi.h>

#include "balloon.h"
#include "theme.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

#define BALLOON_FONT (&lv_font_montserrat_12)

/* The icon box. Fixed rather than scaled with CONFIG_ZD_TOUCH_SLOP_PX, because
 * slop sizes things a finger has to hit and nothing here is clickable. 40x64 is
 * the smallest at which the two loops still read as a paperclip rather than as
 * a blob behind a pair of eyes.
 */
#define ICON_W 40
#define ICON_H 64
#define ICON_GAP 4

/* Wire thickness of the paperclip. Three is too thin to see the loop; five
 * closes the gap between the two loops at this size.
 */
#define WIRE 5

/* Office-assistant yellow. Not in theme.h with the system palette on purpose:
 * that palette is the Win95 *system* one, every entry of which is a real
 * setting from the era's control panel. This is the colour of a balloon, which
 * was never a system colour, and putting it there would imply a theming
 * relationship that does not exist.
 */
#define C_BALLOON      0xFFFFCC
#define C_BALLOON_EDGE 0x000000

/* The clip. Two periwinkles: the near limb light, the far limb darker, which is
 * the only depth cue available without gradients.
 *
 * Darker than the reference art, on purpose. The clip is drawn on the window's
 * own #C0C0C0 face, and the periwinkle these are tinted from disappears against
 * it -- the first build had a paperclip nobody could see. Contrast against the
 * background it actually sits on beats fidelity to a colour picked off a PNG.
 */
#define C_CLIP      0x7676A8
#define C_CLIP_DARK 0x4C4C78

#define C_EYE   0xFFFFFF
#define C_PUPIL 0x000000

struct zd_balloon {
	lv_obj_t *view;
	lv_obj_t *label;
	uint32_t icon;
	uint32_t state;
	bool used;
};

static struct zd_balloon balloons[CONFIG_ZD_MAX_BALLOONS];
static uint32_t live;

int16_t zd_balloon_icon_width(void)
{
	return ICON_W + ICON_GAP;
}

int16_t zd_balloon_icon_height(void)
{
	return ICON_H;
}

/* --- drawing ---------------------------------------------------------------------- */

/** A filled rectangle, given a corner radius and a colour. */
static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
		 uint32_t rgb, int32_t radius)
{
	lv_draw_rect_dsc_t dsc;
	lv_area_t area = { .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2 };

	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_color = lv_color_hex(rgb);
	dsc.bg_opa = LV_OPA_COVER;
	dsc.border_width = 0;
	dsc.radius = radius;

	lv_draw_rect(layer, &dsc, &area);
}

/** An unfilled rounded rectangle: the only wire-drawing primitive here. */
static void wire(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
		 uint32_t rgb, int32_t radius, int32_t width)
{
	lv_draw_rect_dsc_t dsc;
	lv_area_t area = { .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2 };

	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_opa = LV_OPA_TRANSP;
	dsc.border_color = lv_color_hex(rgb);
	dsc.border_width = width;
	dsc.border_opa = LV_OPA_COVER;
	dsc.radius = radius;

	lv_draw_rect(layer, &dsc, &area);
}

/**
 * One eyebrow, as three stepped rectangles.
 *
 * A brow is a short thick stroke at an angle, and there is no rotation in the
 * draw layer -- so it is drawn the way pixel art draws it, as a staircase. At
 * three pixels of rise over fourteen of run this reads as a curve rather than
 * as steps.
 *
 * @param dir +1 slopes down to the right, -1 down to the left.
 */
static void brow(lv_layer_t *layer, int32_t x, int32_t y, int32_t dir)
{
	for (int32_t i = 0; i < 3; i++) {
		int32_t sx = x + dir * i * 6;
		int32_t sy = y + i * 2;

		fill(layer, MIN(sx, sx + dir * 6), sy, MAX(sx, sx + dir * 6), sy + 2,
		     C_PUPIL, 1);
	}
}

/**
 * The paperclip.
 *
 * Two stadiums -- rounded rectangles whose radius is half their width -- offset
 * vertically. That is genuinely what a paperclip is: one wire bent into two
 * nested U shapes, the outer opening downward and the inner upward. Then the
 * eyes go on top, which is the part anybody actually recognises.
 */
static void draw_clip(lv_layer_t *layer, int32_t ox, int32_t oy, uint32_t state)
{
	int32_t lean = (state == ZD_ICON_STATE_BUSY) ? 2 : 0;

	/* Outer loop, opening downward. Drawn first so the inner one and the
	 * eyes sit over it.
	 */
	wire(layer, ox + 3 + lean, oy + 2, ox + 33 + lean, oy + 48, C_CLIP_DARK, 15, WIRE);

	/* Inner loop, opening upward and shifted down: the second bend. */
	wire(layer, ox + 11 + lean, oy + 15, ox + 29 + lean, oy + 62, C_CLIP, 9, WIRE);

	/* A stub of the near limb crossing the top, which is what stops the two
	 * stadiums reading as two separate rings.
	 */
	fill(layer, ox + 18 + lean, oy + 7, ox + 18 + WIRE - 1 + lean, oy + 24, C_CLIP, 1);

	if (state == ZD_ICON_STATE_SAD) {
		/* Eyes shut: two flat bars, which is legible at a glance from
		 * across the screen in a way that a different pupil is not.
		 */
		fill(layer, ox + 1, oy + 24, ox + 19, oy + 27, C_PUPIL, 1);
		fill(layer, ox + 20, oy + 26, ox + 38, oy + 29, C_PUPIL, 1);
		brow(layer, ox + 3, oy + 12, 1);
		brow(layer, ox + 36, oy + 14, -1);
		return;
	}

	/* Whites. The right eye overlaps the left, as in the original -- the two
	 * are not on the same plane. Sat low enough that the top of the outer
	 * loop still shows above them, which is what makes it read as a
	 * paperclip with eyes rather than as a pair of eyes.
	 */
	fill(layer, ox + 1, oy + 16, ox + 20, oy + 38, C_EYE, LV_RADIUS_CIRCLE);
	wire(layer, ox + 1, oy + 16, ox + 20, oy + 38, C_PUPIL, LV_RADIUS_CIRCLE, 2);
	fill(layer, ox + 19, oy + 19, ox + 38, oy + 41, C_EYE, LV_RADIUS_CIRCLE);
	wire(layer, ox + 19, oy + 19, ox + 38, oy + 41, C_PUPIL, LV_RADIUS_CIRCLE, 2);

	if (state == ZD_ICON_STATE_BLINK) {
		/* Lids, drawn over the top two thirds of each eye. Cheaper than a
		 * second set of shapes and they cannot drift out of alignment
		 * with the eye they belong to.
		 */
		fill(layer, ox + 2, oy + 17, ox + 19, oy + 31, C_PUPIL, 3);
		fill(layer, ox + 20, oy + 20, ox + 37, oy + 34, C_PUPIL, 3);
	} else {
		int32_t look = (state == ZD_ICON_STATE_BUSY) ? 4 : 0;

		fill(layer, ox + 6 + look, oy + 22, ox + 16 + look, oy + 33, C_PUPIL,
		     LV_RADIUS_CIRCLE);
		fill(layer, ox + 24 + look, oy + 25, ox + 34 + look, oy + 36, C_PUPIL,
		     LV_RADIUS_CIRCLE);
	}

	brow(layer, ox + 3, oy + 9, 1);
	brow(layer, ox + 36, oy + 12, -1);
}

/** The information and warning icons, so ZD_ICON_CLIP is not a special case. */
static void draw_glyph(lv_layer_t *layer, int32_t ox, int32_t oy, const char *ch,
		       uint32_t rgb)
{
	lv_draw_label_dsc_t dsc;
	lv_area_t area = {
		.x1 = ox + 2, .y1 = oy + 18, .x2 = ox + ICON_W - 2, .y2 = oy + 50
	};

	fill(layer, ox + 5, oy + 16, ox + 35, oy + 46, rgb, LV_RADIUS_CIRCLE);
	wire(layer, ox + 5, oy + 16, ox + 35, oy + 46, C_PUPIL, LV_RADIUS_CIRCLE, 2);

	lv_draw_label_dsc_init(&dsc);
	dsc.font = BALLOON_FONT;
	dsc.color = lv_color_hex(0xFFFFFF);
	dsc.align = LV_TEXT_ALIGN_CENTER;
	dsc.opa = LV_OPA_COVER;
	dsc.text = ch;
	area.y1 += (30 - lv_font_get_line_height(BALLOON_FONT)) / 2;
	lv_draw_label(layer, &dsc, &area);
}

static void balloon_draw_cb(lv_event_t *e)
{
	struct zd_balloon *b = lv_event_get_user_data(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_obj_t *obj = lv_event_get_target_obj(e);
	lv_area_t c;
	int32_t panel_x1;

	if (b == NULL || !b->used) {
		return;
	}

	lv_obj_get_coords(obj, &c);

	panel_x1 = c.x1;
	if (b->icon != ZD_ICON_NONE && (c.y2 - c.y1 + 1) >= ICON_H) {
		panel_x1 += ICON_W + ICON_GAP;
	}

	/* The panel. A one-pixel black edge rather than a bevel: this is not a
	 * control, and giving it the raised bevel every button has would invite
	 * the user to press it.
	 */
	fill(layer, panel_x1, c.y1, c.x2, c.y2, C_BALLOON, 6);
	wire(layer, panel_x1, c.y1, c.x2, c.y2, C_BALLOON_EDGE, 6, 1);

	if (panel_x1 == c.x1) {
		return; /* no room for an icon, or none asked for */
	}

	switch (b->icon) {
	case ZD_ICON_CLIP:
		draw_clip(layer, c.x1, c.y1, b->state);
		break;
	case ZD_ICON_INFO:
		draw_glyph(layer, c.x1, c.y1, "i", 0x000080);
		break;
	case ZD_ICON_WARN:
		draw_glyph(layer, c.x1, c.y1, "!", 0x808000);
		break;
	default:
		break;
	}
}

/* --- lifecycle -------------------------------------------------------------------- */

static void view_deleted(lv_event_t *e)
{
	struct zd_balloon *b = lv_event_get_user_data(e);

	if (b == NULL || !b->used) {
		return;
	}

	b->used = false;
	b->view = NULL;
	b->label = NULL;
	live--;
}

/** Put the label inside the panel, allowing for the icon column. */
static void relayout(struct zd_balloon *b, int16_t w, int16_t h)
{
	int16_t inset = 0;

	if (b->icon != ZD_ICON_NONE && h >= ICON_H) {
		inset = ICON_W + ICON_GAP;
	}

	lv_obj_set_pos(b->label, inset + 6, 5);
	lv_obj_set_width(b->label, w - inset - 12);
	lv_obj_set_height(b->label, h - 10);
}

struct zd_balloon *zd_balloon_create(lv_obj_t *parent, int16_t x, int16_t y, int16_t w,
				     int16_t h, uint32_t icon)
{
	struct zd_balloon *b = NULL;

	if (parent == NULL || w <= 0 || h <= 0) {
		return NULL;
	}

	for (size_t i = 0; i < ARRAY_SIZE(balloons); i++) {
		if (!balloons[i].used) {
			b = &balloons[i];
			break;
		}
	}

	if (b == NULL) {
		LOG_WRN("balloon table full (%d)", CONFIG_ZD_MAX_BALLOONS);
		return NULL;
	}

	b->view = lv_obj_create(parent);
	if (b->view == NULL) {
		return NULL;
	}

	/* Transparent: everything visible is drawn in the callback, and a
	 * background here would show through the panel's rounded corners.
	 */
	lv_obj_remove_style_all(b->view);
	lv_obj_set_pos(b->view, x, y);
	lv_obj_set_size(b->view, w, h);
	lv_obj_remove_flag(b->view, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(b->view, LV_OBJ_FLAG_CLICKABLE);

	b->label = lv_label_create(b->view);
	if (b->label == NULL) {
		lv_obj_delete(b->view);
		b->view = NULL;
		return NULL;
	}

	lv_obj_remove_style_all(b->label);
	lv_obj_set_style_text_font(b->label, BALLOON_FONT, LV_PART_MAIN);
	lv_obj_set_style_text_color(b->label, lv_color_hex(ZD_C_TEXT), LV_PART_MAIN);
	lv_label_set_long_mode(b->label, LV_LABEL_LONG_WRAP);
	lv_label_set_text(b->label, "");

	b->icon = icon;
	b->state = ZD_ICON_STATE_NORMAL;
	b->used = true;
	live++;

	relayout(b, w, h);

	/* DRAW_MAIN, not DRAW_POST. cellgrid.c uses POST because it has no
	 * children and nothing can be covered; this widget has a label, and POST
	 * runs after children are drawn -- so the panel was painted straight
	 * over its own text. The first build of this drew a perfect empty
	 * balloon.
	 */
	lv_obj_add_event_cb(b->view, balloon_draw_cb, LV_EVENT_DRAW_MAIN, b);
	lv_obj_add_event_cb(b->view, view_deleted, LV_EVENT_DELETE, b);

	return b;
}

void zd_balloon_destroy(struct zd_balloon *b)
{
	if (b != NULL && b->used && b->view != NULL) {
		lv_obj_delete(b->view); /* view_deleted() does the bookkeeping */
	}
}

int zd_balloon_set_text(struct zd_balloon *b, const char *text)
{
	if (b == NULL || !b->used) {
		return -EINVAL;
	}

	lv_label_set_text(b->label, text != NULL ? text : "");

	return 0;
}

int zd_balloon_set_geometry(struct zd_balloon *b, int16_t x, int16_t y, int16_t w,
			    int16_t h)
{
	if (b == NULL || !b->used || w <= 0 || h <= 0) {
		return -EINVAL;
	}

	lv_obj_set_pos(b->view, x, y);
	lv_obj_set_size(b->view, w, h);
	relayout(b, w, h);
	lv_obj_invalidate(b->view);

	return 0;
}

int zd_balloon_set_icon(struct zd_balloon *b, uint32_t icon, uint32_t state)
{
	if (b == NULL || !b->used) {
		return -EINVAL;
	}

	if (icon > ZD_ICON_WARN || state > ZD_ICON_STATE_SAD) {
		return -EINVAL;
	}

	/* Only redraw if something changed. This is called from a 400 ms
	 * animation, and an invalidate that changes no pixels still costs a
	 * repaint of the whole balloon.
	 */
	if (b->icon == icon && b->state == state) {
		return 0;
	}

	b->icon = icon;
	b->state = state;
	lv_obj_invalidate(b->view);

	return 0;
}

lv_obj_t *zd_balloon_obj(const struct zd_balloon *b)
{
	return (b != NULL && b->used) ? b->view : NULL;
}

uint32_t zd_balloon_live_count(void)
{
	return live;
}
