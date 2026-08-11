/*
 * zephyr-desktop — taskbar contents.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "taskbar.h"
#include "desktop.h"
#include "../chrome/theme.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

#define LAUNCHER_W 58
#define CLOCK_W    56
#define ITEM_H     (ZD_TASKBAR_H - 8)
#define ITEM_Y     4
#define EDGE_PAD   3
#define GAP        3 /**< between the launcher/clock and the window list */

/*
 * qemu_cortex_a53 has no RTC node and Zephyr has no PL031 driver, so there is
 * no wall clock to read. Rather than show 00:00 since boot -- which reads as a
 * stopwatch, not a desktop -- the clock counts up from a fixed start time.
 * Swapping in rtc_get_time() on hardware means replacing this one function.
 */
#define CLOCK_BASE_HOUR 9
#define CLOCK_BASE_MIN  41

static lv_obj_t *launcher_btn;
static lv_obj_t *clock_label;
static zd_launcher_cb_t launcher_cb;
static void *launcher_cb_arg;

static void clock_now(int *hour, int *minute)
{
	int64_t total = (int64_t)CLOCK_BASE_HOUR * 60 + CLOCK_BASE_MIN
			+ k_uptime_get() / (60 * MSEC_PER_SEC);

	*hour = (int)((total / 60) % 24);
	*minute = (int)(total % 60);
}

static void clock_tick(lv_timer_t *timer)
{
	lv_obj_t *label = lv_timer_get_user_data(timer);
	int hour;
	int minute;

	clock_now(&hour, &minute);
	lv_label_set_text_fmt(label, "%d:%02d", hour == 0 ? 12 : (hour > 12 ? hour - 12 : hour),
			      minute);
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
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);

	out->x1 = EDGE_PAD + LAUNCHER_W + GAP;
	out->y1 = ITEM_Y;
	out->x2 = screen_w - CLOCK_W - EDGE_PAD - GAP - 1;
	out->y2 = ITEM_Y + ITEM_H - 1;
}
