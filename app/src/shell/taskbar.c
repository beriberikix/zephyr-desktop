/*
 * zephyr-desktop — taskbar contents.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "taskbar.h"
#include "desktop.h"
#include "osk.h"
#include "../chrome/theme.h"
#include "../host/clock.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

#define LAUNCHER_W 58
#define CLOCK_W    56
#define OSK_W      34
#define ITEM_H     (ZD_TASKBAR_H - 8)
#define ITEM_Y     4
#define EDGE_PAD   3
#define GAP        3 /**< between the launcher/clock and the window list */

static lv_obj_t *launcher_btn;
static lv_obj_t *clock_label;
static lv_obj_t *osk_btn;
static zd_launcher_cb_t launcher_cb;
static void *launcher_cb_arg;

/** Screen x of the leftmost piece of furniture on the right-hand end. */
static int32_t right_group_x(void)
{
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
	int32_t x = screen_w - CLOCK_W - EDGE_PAD;

	if (IS_ENABLED(CONFIG_ZD_OSK)) {
		x -= OSK_W + GAP;
	}

	return x;
}

/*
 * The time itself comes from host/clock.c, not from here.
 *
 * It used to be four lines of arithmetic in this file, which was fine until
 * something else wanted the same number: Notepad's Time/Date. Two independent
 * fictions disagreeing on one screen is worse than one fiction, so there is now
 * one, and a board with a real RTC fixes both at once.
 */
static void clock_tick(lv_timer_t *timer)
{
	lv_obj_t *label = lv_timer_get_user_data(timer);
	struct zd_time now;

	zd_clock_now(&now);
	lv_label_set_text_fmt(label, "%d:%02d",
			      now.hour == 0 ? 12
					    : (now.hour > 12 ? now.hour - 12 : now.hour),
			      now.minute);
}

static void launcher_event(lv_event_t *e)
{
	if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
		return;
	}

	LOG_INF("launcher clicked");

	if (launcher_cb != NULL) {
		launcher_cb(launcher_cb_arg);
	}
}

static void osk_event(lv_event_t *e)
{
	ARG_UNUSED(e);
	zd_osk_toggle();
}

void zd_taskbar_set_osk_active(bool active)
{
	if (osk_btn == NULL) {
		return;
	}

	/* ZD_BEVEL_BUTTON draws CHECKED sunken, the way Win95 drew a toggle
	 * that is on. Nothing invalidates on a state set by code -- LVGL only
	 * repaints when a *style* property depends on the state, and a bevel
	 * drawn in DRAW_POST is invisible to that check -- so say so here.
	 */
	if (active) {
		lv_obj_add_state(osk_btn, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(osk_btn, LV_STATE_CHECKED);
	}

	lv_obj_invalidate(osk_btn);
}

void zd_taskbar_init(lv_obj_t *panel, zd_launcher_cb_t cb, void *cb_arg)
{
	lv_obj_t *label;
	lv_obj_t *clock_box;

	launcher_cb = cb;
	launcher_cb_arg = cb_arg;

	/* --- launcher button, hard left --- */
	launcher_btn = lv_obj_create(panel);
	lv_obj_remove_style_all(launcher_btn);
	lv_obj_add_style(launcher_btn, &zd_style_face, LV_PART_MAIN);
	lv_obj_remove_flag(launcher_btn, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(launcher_btn, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(launcher_btn, LAUNCHER_W, ITEM_H);
	lv_obj_set_pos(launcher_btn, EDGE_PAD, ITEM_Y);
	zd_bevel_attach(launcher_btn, ZD_BEVEL_BUTTON);
	/* Grow the hit target without moving the pixels -- see ZD_TOUCH_SLOP_PX. */
	lv_obj_set_ext_click_area(launcher_btn, CONFIG_ZD_TOUCH_SLOP_PX);
	lv_obj_add_event_cb(launcher_btn, launcher_event, LV_EVENT_CLICKED, NULL);

	label = lv_label_create(launcher_btn);
	lv_label_set_text(label, "Start");
	lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
	lv_obj_set_style_text_color(label, lv_color_hex(ZD_C_TEXT), LV_PART_MAIN);
	lv_obj_center(label);

	/* --- keyboard toggle, just left of the clock --- */
	if (IS_ENABLED(CONFIG_ZD_OSK)) {
		osk_btn = lv_obj_create(panel);
		lv_obj_remove_style_all(osk_btn);
		lv_obj_add_style(osk_btn, &zd_style_face, LV_PART_MAIN);
		lv_obj_remove_flag(osk_btn, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(osk_btn, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(osk_btn, OSK_W, ITEM_H);
		lv_obj_set_pos(osk_btn, right_group_x(), ITEM_Y);
		zd_bevel_attach(osk_btn, ZD_BEVEL_BUTTON);
		/* Slop is safe here and not on the window list: the clock to
		 * its right is not clickable and the strip to its left is
		 * bounded away by GAP, so this is an isolated control in the
		 * sense ext_click_area requires.
		 */
		lv_obj_set_ext_click_area(osk_btn, CONFIG_ZD_TOUCH_SLOP_PX);
		lv_obj_add_event_cb(osk_btn, osk_event, LV_EVENT_CLICKED, NULL);

		label = lv_label_create(osk_btn);
		lv_label_set_text(label, "abc");
		lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
		lv_obj_set_style_text_color(label, lv_color_hex(ZD_C_TEXT), LV_PART_MAIN);
		lv_obj_center(label);
	}

	/* --- clock, hard right, sunken --- */
	clock_box = lv_obj_create(panel);
	lv_obj_remove_style_all(clock_box);
	lv_obj_add_style(clock_box, &zd_style_face, LV_PART_MAIN);
	lv_obj_remove_flag(clock_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(clock_box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(clock_box, CLOCK_W, ITEM_H);
	lv_obj_set_pos(clock_box, lv_display_get_horizontal_resolution(NULL) - CLOCK_W - EDGE_PAD,
		       ITEM_Y);
	zd_bevel_attach(clock_box, ZD_BEVEL_IN);

	clock_label = lv_label_create(clock_box);
	lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_12, LV_PART_MAIN);
	lv_obj_set_style_text_color(clock_label, lv_color_hex(ZD_C_TEXT), LV_PART_MAIN);
	lv_obj_center(clock_label);

	/* Tick every 5s rather than every minute: the label must be correct
	 * shortly after boot and shortly after each rollover, and a 5s timer on
	 * an otherwise idle UI costs nothing.
	 */
	lv_timer_t *timer = lv_timer_create(clock_tick, 5 * MSEC_PER_SEC, clock_label);

	clock_tick(timer);
}

void zd_taskbar_launcher_coords(lv_area_t *out)
{
	lv_obj_get_coords(launcher_btn, out);
}

void zd_taskbar_list_region(lv_area_t *out)
{
	out->x1 = EDGE_PAD + LAUNCHER_W + GAP;
	out->y1 = ITEM_Y;
	out->x2 = right_group_x() - GAP - 1;

	/*
	 * Flush with the bottom of the panel -- and so with the bottom of the
	 * screen -- rather than inset like the launcher and the clock.
	 *
	 * Those two get their touch allowance from ext_click_area, which these
	 * cannot have: they sit edge to edge in a row, and slop on adjacent
	 * controls overlaps, handing every tap to the rightmost one. Height is
	 * the only allowance left, and it has to go downward, because that is
	 * the direction this panel's taps miss in -- a press aimed at the
	 * launcher button (drawn to y 236) reads y=239. See docs/hardware.md.
	 */
	out->y2 = ZD_TASKBAR_H - 1;
}
