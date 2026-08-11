/*
 * zephyr-desktop — drag-to-move by the titlebar, drag-to-resize by the grip.
 *
 * Implemented by the WM rather than by any LVGL dragging behaviour, so that
 * hit-test -> focus -> dispatch stays one pipeline the WM owns. client->geom
 * remains authoritative throughout; LVGL is told where the window went, never
 * asked.
 *
 * Move and resize share one handler and are told apart by client->drag_mode.
 * They are the same gesture with a different consequence, and writing them as
 * two state machines would mean two chances to forget PRESS_LOST.
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

/*
 * The mirror of move_to(): the top-left corner is pinned and the bottom-right
 * follows the pointer.
 *
 * Clamped at both ends. The floor is the window minimum, below which the chrome
 * would start overlapping itself; the ceiling keeps the grip on screen, because
 * a window resized until its own resize handle is past the edge cannot be
 * resized back.
 */
static void resize_to(struct zd_client *client, int32_t w, int32_t h)
{
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
	int32_t screen_h = lv_display_get_vertical_resolution(NULL);
	int32_t max_w = screen_w - client->geom.x1;
	int32_t max_h = screen_h - ZD_TASKBAR_H - client->geom.y1;

	w = CLAMP(w, ZD_WIN_MIN_W, MAX(max_w, ZD_WIN_MIN_W));
	h = CLAMP(h, ZD_WIN_MIN_H, MAX(max_h, ZD_WIN_MIN_H));

	client->geom.x2 = client->geom.x1 + w - 1;
	client->geom.y2 = client->geom.y1 + h - 1;

	zd_client_apply_geom(client);
}

static void begin(struct zd_client *client, lv_indev_t *indev, enum zd_drag_mode mode)
{
	lv_indev_get_point(indev, &client->drag_grab);
	client->drag_origin.x = client->geom.x1;
	client->drag_origin.y = client->geom.y1;
	client->drag_size.x = lv_area_get_width(&client->geom);
	client->drag_size.y = lv_area_get_height(&client->geom);
	client->drag_mode = mode;
}

static void finish(struct zd_client *client)
{
	bool was_resize = client->drag_mode == ZD_DRAG_RESIZE;

	client->drag_mode = ZD_DRAG_NONE;

	/* One event for the whole gesture, on release. See ZD_EV_RESIZED: a
	 * zapp callback may touch the filesystem, and on a board where storage
	 * borrows the display's pin, one of those per pointer sample would stop
	 * the screen for the length of the drag.
	 */
	if (was_resize) {
		zd_wm_notify_resized(client);
	}
}

static void drag_event(lv_event_t *e)
{
	struct zd_client *client = lv_event_get_user_data(e);
	lv_indev_t *indev = lv_indev_active();
	lv_point_t point;

	if (client->pending_destroy || indev == NULL) {
		return;
	}

	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		/* current_target, not target: it names the object this callback
		 * is attached to, so a press that bubbled up from a titlebar
		 * child still reads as a titlebar press rather than as whatever
		 * child it started on.
		 */
		begin(client, indev,
		      lv_event_get_current_target_obj(e) == client->grip ? ZD_DRAG_RESIZE
									: ZD_DRAG_MOVE);
		break;

	case LV_EVENT_PRESSING:
		if (client->drag_mode == ZD_DRAG_NONE) {
			break;
		}
		lv_indev_get_point(indev, &point);

		if (client->drag_mode == ZD_DRAG_MOVE) {
			move_to(client,
				client->drag_origin.x + (point.x - client->drag_grab.x),
				client->drag_origin.y + (point.y - client->drag_grab.y));
		} else {
			resize_to(client,
				  client->drag_size.x + (point.x - client->drag_grab.x),
				  client->drag_size.y + (point.y - client->drag_grab.y));
		}
		break;

	case LV_EVENT_RELEASED:
	case LV_EVENT_PRESS_LOST:
		finish(client);
		break;

	default:
		break;
	}
}

static void attach_to(struct zd_client *client, lv_obj_t *obj)
{
	lv_obj_add_event_cb(obj, drag_event, LV_EVENT_PRESSED, client);
	lv_obj_add_event_cb(obj, drag_event, LV_EVENT_PRESSING, client);
	lv_obj_add_event_cb(obj, drag_event, LV_EVENT_RELEASED, client);
	lv_obj_add_event_cb(obj, drag_event, LV_EVENT_PRESS_LOST, client);
}

void zd_wm_drag_attach(struct zd_client *client)
{
	/* Move on the titlebar only: dragging by the content area would fight
	 * with whatever the zapp puts there. Resize on the grip only, for the
	 * same reason -- a zapp owns its content area and nothing the WM does
	 * inside it can be distinguished from a bug in the zapp.
	 */
	attach_to(client, client->titlebar);
	attach_to(client, client->grip);
}
