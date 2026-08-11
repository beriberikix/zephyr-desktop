/*
 * zephyr-desktop — window manager core: creation, the stack, deferred reap.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

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

	/* Newest window goes on top of the stacking list. */
	sys_dlist_prepend(&wm->stack, &client->node);
	live_clients++;

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
	if (wm->focused == client) {
		wm->focused = NULL;
	}
	lv_obj_add_flag(client->frame, LV_OBJ_FLAG_HIDDEN);

	sys_slist_append(&wm->reap_list, &client->reap_node);
	LOG_DBG("window %u '%s' queued for reap", client->id, client->title);
}

void zd_wm_reap(struct zd_wm *wm)
{
	sys_snode_t *node;

	/* Never reap with an app frame on the stack: the return address may
	 * point into text we are about to free.
	 */
	if (wm->in_app_callback > 0) {
		return;
	}

	while ((node = sys_slist_get(&wm->reap_list)) != NULL) {
		struct zd_client *client = CONTAINER_OF(node, struct zd_client, reap_node);

		LOG_DBG("reaping window %u '%s'", client->id, client->title);
		zd_client_destroy_widgets(client);
		k_mem_slab_free(&client_slab, (void *)client);
		live_clients--;
	}
}

uint32_t zd_wm_client_count(const struct zd_wm *wm)
{
	ARG_UNUSED(wm);
	return live_clients;
}
