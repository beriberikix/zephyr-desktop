/*
 * zephyr-desktop — window manager core: creation, the stack, deferred reap.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include "wm.h"
#include "client.h"
#include "../chrome/theme.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

#define DEFAULT_W 200
#define DEFAULT_H 120

/* Windows come from a slab, not the heap: see the comment on struct zd_client. */
K_MEM_SLAB_DEFINE_STATIC(client_slab, sizeof(struct zd_client), CONFIG_ZD_MAX_CLIENTS,
			 sizeof(void *));

static uint32_t live_clients;

void zd_wm_init(struct zd_wm *wm, struct zd_layers *layers)
{
	memset(wm, 0, sizeof(*wm));
	wm->layers = layers;
	wm->next_id = 1;
	sys_dlist_init(&wm->stack);
	sys_slist_init(&wm->reap_list);
}

/* Cascade successive windows so a second one is visibly a second one. */
static void place_default(struct zd_wm *wm, lv_area_t *geom)
{
	int32_t step = (int32_t)((wm->next_id - 1) % 5) * 18;

	geom->x1 = 24 + step;
	geom->y1 = 20 + step;
	geom->x2 = geom->x1 + DEFAULT_W - 1;
	geom->y2 = geom->y1 + DEFAULT_H - 1;
}

struct zd_client *zd_wm_window_create(struct zd_wm *wm, const char *title,
				      const lv_area_t *geom)
{
	struct zd_client *client;
	lv_area_t want;

	if (k_mem_slab_alloc(&client_slab, (void **)&client, K_NO_WAIT) != 0) {
		LOG_ERR("window slab exhausted (%d in use); refusing to create '%s'",
			CONFIG_ZD_MAX_CLIENTS, title != NULL ? title : "?");
		return NULL;
	}

	memset(client, 0, sizeof(*client));

	if (geom == NULL || lv_area_get_width(geom) <= 0 || lv_area_get_height(geom) <= 0) {
		place_default(wm, &want);
	} else {
		want = *geom;
		if (lv_area_get_width(&want) < ZD_WIN_MIN_W) {
			want.x2 = want.x1 + ZD_WIN_MIN_W - 1;
		}
		if (lv_area_get_height(&want) < ZD_WIN_MIN_H) {
			want.y2 = want.y1 + ZD_WIN_MIN_H - 1;
		}
	}

	client->wm = wm;
	client->id = wm->next_id++;
	client->geom = want;
	if (title != NULL) {
		strncpy(client->title, title, ZD_TITLE_MAX - 1);
	}

	zd_client_build(client, wm->layers->windows);
	zd_wm_client_attach_events(client);

	/* Newest window goes on top of the stacking list. It is deliberately NOT
	 * focused here: the caller still has to attach ownership and a handle,
	 * and focusing first means the owning zapp's ZD_EV_WINDOW_FOCUS is
	 * dispatched against a window it does not yet have a handle for, and is
	 * silently dropped. Creation and focus are separate policies.
	 */
	sys_dlist_prepend(&wm->stack, &client->node);
	live_clients++;
	zd_wm_restack(wm);
	zd_wm_notify_list_changed(wm);

	LOG_DBG("window %u '%s' created at %d,%d %dx%d", client->id, client->title,
		client->geom.x1, client->geom.y1, lv_area_get_width(&client->geom),
		lv_area_get_height(&client->geom));

	return client;
}

void zd_wm_window_set_title(struct zd_client *client, const char *title)
{
	strncpy(client->title, title != NULL ? title : "", ZD_TITLE_MAX - 1);
	client->title[ZD_TITLE_MAX - 1] = '\0';
	lv_label_set_text(client->title_label, client->title);
	zd_wm_notify_list_changed(client->wm);
}

void zd_wm_notify_list_changed(struct zd_wm *wm)
{
	if (wm->on_client_list_changed != NULL) {
		wm->on_client_list_changed(wm);
	}
}

int zd_wm_window_set_geometry(struct zd_client *client, int16_t x, int16_t y, int16_t w,
			      int16_t h)
{
	int32_t width = w > 0 ? w : lv_area_get_width(&client->geom);
	int32_t height = h > 0 ? h : lv_area_get_height(&client->geom);
	bool resized;

	if (client->pending_destroy) {
		return -EINVAL;
	}

	width = MAX(width, ZD_WIN_MIN_W);
	height = MAX(height, ZD_WIN_MIN_H);

	resized = width != lv_area_get_width(&client->geom) ||
		  height != lv_area_get_height(&client->geom);

	client->geom.x1 = x;
	client->geom.y1 = y;
	client->geom.x2 = x + width - 1;
	client->geom.y2 = y + height - 1;
	zd_client_apply_geom(client);

	if (resized) {
		zd_wm_notify_resized(client);
	}

	return 0;
}

/*
 * Tell the owner the content area settled at a new size.
 *
 * The content area, not the frame: that is the rectangle a zapp lays widgets
 * out in, and the only one it should ever have to reason about. Shared with
 * drag.c, which calls this once on release rather than per pointer sample.
 */
void zd_wm_notify_resized(struct zd_client *client)
{
	struct zd_wm *wm = client->wm;
	int16_t w;
	int16_t h;

	if (wm->on_client_resized == NULL || client->pending_destroy) {
		return;
	}

	/* Derived from the model rather than read off the content object: LVGL
	 * has been told the new size but has not laid it out yet, so asking it
	 * here would hand the zapp the size the window used to be.
	 */
	zd_client_content_size(client, &w, &h);
	wm->on_client_resized(client, w, h);
}

void zd_wm_window_minimize(struct zd_client *client)
{
	struct zd_wm *wm = client->wm;

	if (client->pending_destroy || client->minimized) {
		return;
	}

	client->minimized = true;

	/* Any drag in progress is over: the object the pointer was holding is
	 * about to stop being hit-tested, so RELEASED may never arrive.
	 */
	client->drag_mode = ZD_DRAG_NONE;

	/* Restack first, so the frame is hidden before focus is handed on --
	 * otherwise zd_wm_top() would still be free to pick this client back up.
	 */
	zd_wm_restack(wm);

	if (wm->focused == client) {
		wm->focused = NULL;
		client->focused = false;
		zd_wm_focus(wm, zd_wm_top(wm));
	}

	if (wm->on_client_minimized != NULL) {
		wm->on_client_minimized(client, true);
	}
	zd_wm_notify_list_changed(wm);

	LOG_DBG("window %u '%s' minimised", client->id, client->title);
}

void zd_wm_window_restore(struct zd_client *client)
{
	struct zd_wm *wm = client->wm;

	if (client->pending_destroy || !client->minimized) {
		return;
	}

	client->minimized = false;

	/* Raise as well as unhide. Restoring a window and leaving it behind the
	 * one that was covering it looks exactly like the restore not working.
	 */
	zd_wm_raise(wm, client);
	zd_wm_restack(wm);
	zd_wm_focus(wm, client);

	if (wm->on_client_minimized != NULL) {
		wm->on_client_minimized(client, false);
	}
	zd_wm_notify_list_changed(wm);

	LOG_DBG("window %u '%s' restored", client->id, client->title);
}

void zd_wm_window_close_request(struct zd_client *client)
{
	struct zd_wm *wm = client->wm;

	if (client->pending_destroy) {
		return;
	}

	/* A second ask is a force quit. The close box is the only UI this needs:
	 * click it again and the window goes, whatever the zapp is doing.
	 */
	if (client->close_requested) {
		LOG_WRN("window %u '%s': closing on the second request", client->id,
			client->title);
		zd_wm_window_close(client);
		return;
	}

	/* Nobody to ask -- a desktop-internal window, or a zapp with no event
	 * callback. Asking a hook that cannot answer would just stall the close
	 * for the whole grace period.
	 */
	if (wm->on_client_close_request == NULL || !wm->on_client_close_request(client)) {
		zd_wm_window_close(client);
		return;
	}

	client->close_requested = true;
	client->close_deadline = k_uptime_get() + CONFIG_ZD_CLOSE_GRACE_MS;
	LOG_DBG("window %u '%s': asked its zapp to close", client->id, client->title);
}

void zd_wm_window_close(struct zd_client *client)
{
	struct zd_wm *wm = client->wm;

	if (client->pending_destroy) {
		return;
	}
	client->pending_destroy = true;

	/* Unlink from everything the live desktop consults, immediately: from
	 * here on the window must be invisible to focus, stacking and dispatch,
	 * even though its LVGL objects still exist until the reap.
	 */
	sys_dlist_remove(&client->node);
	lv_obj_add_flag(client->frame, LV_OBJ_FLAG_HIDDEN);

	if (wm->focused == client) {
		/* Hand focus to whatever is now on top, so closing the front
		 * window leaves the desktop focused rather than blank.
		 */
		wm->focused = NULL;
		client->focused = false;
		zd_wm_focus(wm, zd_wm_top(wm));
	}

	sys_slist_append(&wm->reap_list, &client->reap_node);
	zd_wm_notify_list_changed(wm);
	LOG_DBG("window %u '%s' queued for reap", client->id, client->title);
}

/*
 * Close anything whose zapp was asked politely and did not answer in time.
 *
 * The grace period is what makes ZD_EV_WINDOW_CLOSE_REQUEST safe to send at
 * all: without it, a zapp that ignores the event -- or crashes handling it --
 * owns a window the user can no longer get rid of.
 */
static void sweep_close_deadlines(struct zd_wm *wm)
{
	struct zd_client *client;
	struct zd_client *next;
	int64_t now = k_uptime_get();

	SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&wm->stack, client, next, node) {
		if (!client->close_requested || now < client->close_deadline) {
			continue;
		}

		LOG_WRN("window %u '%s' (%s) did not answer the close request in %d ms; "
			"closing it",
			client->id, client->title,
			client->owner != NULL ? "zapp" : "desktop",
			CONFIG_ZD_CLOSE_GRACE_MS);
		zd_wm_window_close(client);
	}
}

void zd_wm_reap(struct zd_wm *wm)
{
	sys_snode_t *node;
	uint32_t reaped = 0;

	/* Never reap with a zapp frame on the stack: the return address may
	 * point into text we are about to free.
	 */
	if (wm->in_zapp_callback > 0) {
		return;
	}

	/* Before the reap, not after: an expired window queued here is destroyed
	 * on this pass rather than lingering until the next one.
	 */
	sweep_close_deadlines(wm);

	while ((node = sys_slist_get(&wm->reap_list)) != NULL) {
		struct zd_client *client = CONTAINER_OF(node, struct zd_client, reap_node);

		LOG_DBG("reaping window %u '%s'", client->id, client->title);
		zd_client_destroy_widgets(client);

		/* Tell the owner after the widgets are gone but before the slab
		 * block is reused, so the loader can drop the zapp's handle and
		 * decide whether the instance still has a reason to exist.
		 */
		if (wm->on_client_destroyed != NULL) {
			wm->on_client_destroyed(client);
		}

		k_mem_slab_free(&client_slab, (void *)client);
		live_clients--;
		reaped++;
	}

	if (reaped > 0) {
		LOG_INF("reaped %u window(s); %u live, %u slab blocks free", reaped,
			live_clients, k_mem_slab_num_free_get(&client_slab));
	}
}

uint32_t zd_wm_client_count(const struct zd_wm *wm)
{
	ARG_UNUSED(wm);
	return live_clients;
}
