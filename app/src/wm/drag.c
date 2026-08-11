/*
 * zephyr-desktop — drag-to-move by the titlebar.
 *
 * Implemented by the WM rather than by any LVGL dragging behaviour, so that
 * hit-test -> focus -> dispatch stays one pipeline the WM owns. client->geom
 * remains authoritative throughout; LVGL is told where the window went, never
 * asked.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wm.h"
#include "client.h"

/* Keep at least this much of the titlebar reachable, so a window can always be
 * dragged back from an edge.
 */
#define KEEP_VISIBLE 60

static void clamp_position(struct zd_client *client, int32_t *x, int32_t *y)
{
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
	int32_t screen_h = lv_display_get_vertical_resolution(NULL);
	int32_t w = lv_area_get_width(&client->geom);
	int32_t usable_h = screen_h - ZD_TASKBAR_H;

	*x = CLAMP(*x, -(w - KEEP_VISIBLE), screen_w - KEEP_VISIBLE);
	/* The titlebar must stay below the top edge and above the taskbar;
	 * the body is free to run off the bottom.
	 */
	*y = CLAMP(*y, 0, usable_h - ZD_TITLEBAR_H);
}

static void move_to(struct zd_client *client, int32_t x, int32_t y)
{
	int32_t w = lv_area_get_width(&client->geom);
	int32_t h = lv_area_get_height(&client->geom);

	clamp_position(client, &x, &y);

	client->geom.x1 = x;
	client->geom.y1 = y;
	client->geom.x2 = x + w - 1;
	client->geom.y2 = y + h - 1;

	zd_client_apply_geom(client);
}

static void titlebar_event(lv_event_t *e)
{
	struct zd_client *client = lv_event_get_user_data(e);
	lv_indev_t *indev = lv_indev_active();
	lv_point_t point;

	if (client->pending_destroy || indev == NULL) {
		return;
	}

	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		lv_indev_get_point(indev, &client->drag_grab);
		client->drag_origin.x = client->geom.x1;
		client->drag_origin.y = client->geom.y1;
		client->dragging = true;
		break;

	case LV_EVENT_PRESSING:
		if (!client->dragging) {
			break;
		}
		lv_indev_get_point(indev, &point);
		move_to(client,
			client->drag_origin.x + (point.x - client->drag_grab.x),
			client->drag_origin.y + (point.y - client->drag_grab.y));
		break;

	case LV_EVENT_RELEASED:
	case LV_EVENT_PRESS_LOST:
		client->dragging = false;
		break;

	default:
		break;
	}
}

void zd_wm_drag_attach(struct zd_client *client)
{
	/* On the titlebar only: dragging by the content area would fight with
	 * whatever the zapp puts there.
	 */
	lv_obj_add_event_cb(client->titlebar, titlebar_event, LV_EVENT_PRESSED, client);
	lv_obj_add_event_cb(client->titlebar, titlebar_event, LV_EVENT_PRESSING, client);
	lv_obj_add_event_cb(client->titlebar, titlebar_event, LV_EVENT_RELEASED, client);
	lv_obj_add_event_cb(client->titlebar, titlebar_event, LV_EVENT_PRESS_LOST, client);
}
