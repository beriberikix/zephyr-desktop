/*
 * zephyr-desktop — hit-test -> focus -> dispatch.
 *
 * LVGL supplies the input device and the hit test; the WM supplies the policy.
 * Every client frame carries exactly one WM callback and every child bubbles to
 * it, so "press inside window X" is a single code path regardless of which
 * widget was actually hit. A future keyboard or encoder modality routes through
 * the same funnel rather than inheriting per-widget LVGL behaviour.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>

#include "wm.h"
#include "../chrome/titlebar.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

static void apply_focus_look(struct zd_client *client)
{
	zd_titlebar_set_active(client->titlebar, client->focused);

	if (client->wm->on_client_focus != NULL) {
		client->wm->on_client_focus(client, client->focused);
	}
}

void zd_wm_focus(struct zd_wm *wm, struct zd_client *client)
{
	if (wm->focused == client) {
		return;
	}

	if (wm->focused != NULL) {
		wm->focused->focused = false;
		apply_focus_look(wm->focused);
	}

	wm->focused = client;

	if (client != NULL) {
		client->focused = true;
		apply_focus_look(client);
		LOG_DBG("focus -> window %u '%s'", client->id, client->title);
	} else {
		LOG_DBG("focus -> none");
	}

	/* The taskbar draws the focused window's button pressed, so focus is a
	 * list change even though the set of windows did not change.
	 */
	zd_wm_notify_list_changed(wm);
}

/* The one callback every frame gets. Children bubble here. */
static void frame_event(lv_event_t *e)
{
	struct zd_client *client = lv_event_get_user_data(e);
	struct zd_wm *wm = client->wm;

	if (lv_event_get_code(e) != LV_EVENT_PRESSED || client->pending_destroy) {
		return;
	}

	zd_wm_raise(wm, client);
	zd_wm_focus(wm, client);
}

/*
 * A click landing in the content area, translated for the zapp.
 *
 * Separate from frame_event because the frame only ever means "raise and
 * focus": it fires on PRESSED, for the chrome as much as the content, and a
 * zapp must not be told the user clicked in its window when they were actually
 * dragging its titlebar or hitting its close box.
 */
static void content_event(lv_event_t *e)
{
	struct zd_client *client = lv_event_get_user_data(e);
	lv_indev_t *indev = lv_indev_active();
	lv_point_t point;
	lv_area_t area;

	if (client->pending_destroy || indev == NULL ||
	    client->wm->on_client_click == NULL) {
		return;
	}

	lv_indev_get_point(indev, &point);
	lv_obj_get_coords(client->content, &area);

	client->wm->on_client_click(client, (int16_t)(point.x - area.x1),
				    (int16_t)(point.y - area.y1));
}

static void close_event(lv_event_t *e)
{
	struct zd_client *client = lv_event_get_user_data(e);

	/* The polite form: the owning zapp is asked first and gets a bounded
	 * grace period. Either way nothing is destroyed here -- the reap at the
	 * top of the desktop loop does the deleting, because destroying now
	 * would free the LVGL object whose event dispatch we are inside.
	 */
	zd_wm_window_close_request(client);
}

static void minimize_event(lv_event_t *e)
{
	struct zd_client *client = lv_event_get_user_data(e);

	/* No handshake here, and none wanted: minimising destroys nothing, so
	 * there is nothing for a zapp to object to. It is told after the fact.
	 */
	zd_wm_window_minimize(client);
}

static void desktop_event(lv_event_t *e)
{
	struct zd_wm *wm = lv_event_get_user_data(e);

	if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
		zd_wm_focus(wm, NULL);
	}
}

void zd_wm_client_attach_events(struct zd_client *client)
{
	static const lv_obj_flag_t bubble = LV_OBJ_FLAG_EVENT_BUBBLE;

	lv_obj_add_event_cb(client->frame, frame_event, LV_EVENT_PRESSED, client);

	/* Bubble everything inside the window up to the frame, so clicking a
	 * label or the content area raises and focuses just like clicking the
	 * frame itself does.
	 */
	lv_obj_add_flag(client->titlebar, bubble);
	lv_obj_add_flag(client->title_label, bubble);
	lv_obj_add_flag(client->min_btn, bubble);
	lv_obj_add_flag(client->close_btn, bubble);
	lv_obj_add_flag(client->content, bubble);
	lv_obj_add_flag(client->grip, bubble);

	lv_obj_add_event_cb(client->min_btn, minimize_event, LV_EVENT_CLICKED, client);
	lv_obj_add_event_cb(client->close_btn, close_event, LV_EVENT_CLICKED, client);
	lv_obj_add_event_cb(client->content, content_event, LV_EVENT_CLICKED, client);

	zd_wm_drag_attach(client);
}

void zd_wm_desktop_attach_events(struct zd_wm *wm)
{
	lv_obj_add_event_cb(wm->layers->desktop, desktop_event, LV_EVENT_PRESSED, wm);
}
