/*
 * zephyr-desktop — the taskbar's window list.
 *
 * One button per window, in the strip between the launcher and the clock. It is
 * what makes minimise usable: an unmapped window draws nowhere, so without a
 * list entry it is simply gone.
 *
 * The row is rebuilt wholesale rather than patched. With CONFIG_ZD_MAX_CLIENTS
 * windows the cost is irrelevant, and it means the button row is derived from
 * wm->stack the same way LVGL's child order is -- one direction, no state of its
 * own to drift out of agreement with the WM.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "tasklist.h"
#include "taskbar.h"
#include "desktop.h"
#include "../chrome/theme.h"
#include "../wm/wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

#define BTN_GAP 2
#define BTN_MIN_W 40

static struct {
	lv_obj_t *row;
	struct zd_wm *wm;
	/* The row's size, kept here rather than read back from LVGL. The first
	 * rebuild can run before LVGL's first layout pass, and lv_obj_get_width()
	 * would answer 0 -- every button laid out on top of every other one, in
	 * the one case nobody tests twice. Same reason the WM derives a resized
	 * window's content size from its own model.
	 */
	int32_t w;
	int32_t h;
	bool dirty;
} list;

/*
 * Is this pointer still a live window?
 *
 * A button holds a struct zd_client *, and clients come from a slab that is
 * reused. The desktop loop reaps windows before it rebuilds this row, so a
 * button for a dead client should never survive long enough to be clicked --
 * but "should never" plus a recycled allocation is how use-after-free gets
 * written. wm->stack is the authority, it is at most CONFIG_ZD_MAX_CLIENTS
 * long, and checking it costs nothing on a click.
 */
static bool still_live(struct zd_client *want)
{
	struct zd_client *client;

	SYS_DLIST_FOR_EACH_CONTAINER(&list.wm->stack, client, node) {
		if (client == want) {
			return !client->pending_destroy;
		}
	}

	return false;
}

static void button_clicked(lv_event_t *e)
{
	struct zd_client *client = lv_event_get_user_data(e);

	if (!still_live(client)) {
		return;
	}

	/* The Win95 rule. Clicking the button of the window you are already
	 * looking at puts it away; clicking any other one brings it to you.
	 */
	if (client->minimized) {
		zd_wm_window_restore(client);
	} else if (client->focused) {
		zd_wm_window_minimize(client);
	} else {
		zd_wm_raise(client->wm, client);
		zd_wm_focus(client->wm, client);
	}
}

static void add_button(struct zd_client *client, int32_t x, int32_t w)
{
	lv_obj_t *btn = lv_obj_create(list.row);
	lv_obj_t *label;
	bool active = client->focused && !client->minimized;

	lv_obj_remove_style_all(btn);
	lv_obj_add_style(btn, &zd_style_face, LV_PART_MAIN);
	lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(btn, w, list.h);
	lv_obj_set_pos(btn, x, 0);

	/* The focused window's button is held down. Buttons sit edge to edge in
	 * a row, so unlike the launcher they get no ext_click_area -- expanding
	 * contiguous items makes them overlap, and LVGL awards an overlap to the
	 * last child, which would make every tap land on the rightmost window.
	 * See the same note in launcher.c.
	 */
	zd_bevel_attach(btn, active ? ZD_BEVEL_IN : ZD_BEVEL_BUTTON);
	lv_obj_add_event_cb(btn, button_clicked, LV_EVENT_CLICKED, client);

	label = lv_label_create(btn);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(label, client->title);
	lv_obj_set_width(label, MAX(w - 8, 1));
	lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
	/* Minimised reads as greyed, the way a disabled control does -- the
	 * window is still there, it just is not showing.
	 */
	lv_obj_set_style_text_color(
		label, lv_color_hex(client->minimized ? ZD_C_SHADOW : ZD_C_TEXT),
		LV_PART_MAIN);
	lv_obj_set_pos(label, 4 + (active ? 1 : 0), (list.h - 12) / 2 + (active ? 1 : 0));
}

static void rebuild(void)
{
	struct zd_client *client;
	uint32_t count = 0;
	int32_t width;
	int32_t x = 0;

	lv_obj_clean(list.row);

	/* Every client, mapped or not -- an unmapped one is exactly what this
	 * row exists to keep reachable.
	 */
	SYS_DLIST_FOR_EACH_CONTAINER(&list.wm->stack, client, node) {
		if (!client->pending_destroy) {
			count++;
		}
	}

	if (count == 0) {
		return;
	}

	/* Share the strip out, capped so two windows do not get half the screen
	 * each. There is no floor: below BTN_MIN_W the label is unreadable, but
	 * a too-small button is still clickable, whereas a row that overflows
	 * the strip puts windows off the end where they cannot be reached at
	 * all. Shrinking is the better failure.
	 */
	width = (list.w - (int32_t)(count - 1) * BTN_GAP) / (int32_t)count;
	width = MIN(width, CONFIG_ZD_TASKLIST_BTN_MAX_W);

	if (width < BTN_MIN_W) {
		LOG_DBG("tasklist: %u window(s) squeezed to %d px", count, width);
	}

	/* Bottom of the stack first, so a button keeps its place as windows are
	 * raised past each other. Ordering the row by z-order would shuffle it
	 * under the user's finger on every click.
	 */
	SYS_DLIST_FOR_EACH_CONTAINER(&list.wm->stack, client, node) {
		if (client->pending_destroy) {
			continue;
		}
		add_button(client, x, width);
		x += width + BTN_GAP;
	}
}

void zd_tasklist_invalidate(struct zd_wm *wm)
{
	ARG_UNUSED(wm);
	list.dirty = true;
}

void zd_tasklist_reap(void)
{
	if (!list.dirty || list.row == NULL) {
		return;
	}

	list.dirty = false;
	rebuild();
}

void zd_tasklist_init(lv_obj_t *panel, struct zd_wm *wm)
{
	lv_area_t region;

	list.wm = wm;

	zd_taskbar_list_region(&region);

	/* A bare container, not a bevelled one: the buttons carry the chrome and
	 * a frame around them would eat pixels the CoreS3 cannot spare.
	 */
	list.row = lv_obj_create(panel);
	lv_obj_remove_style_all(list.row);
	lv_obj_remove_flag(list.row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(list.row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_pad_all(list.row, 0, LV_PART_MAIN);
	list.w = lv_area_get_width(&region);
	list.h = lv_area_get_height(&region);
	lv_obj_set_size(list.row, list.w, list.h);
	lv_obj_set_pos(list.row, region.x1, region.y1);

	list.dirty = true;
}
