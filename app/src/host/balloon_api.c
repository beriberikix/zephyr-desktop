/*
 * zephyr-desktop — implementation of the zapp-facing balloon.
 *
 * Thin, like grid_api.c and for the same reason: everything hard about a
 * balloon -- drawing a paperclip out of rounded rectangles, wrapping the text,
 * dropping the icon when the box is too short -- is in chrome/balloon.c. What
 * is left here is a generation-counted handle with an owner check.
 *
 * There is no event path at all, which makes this the simplest widget layer in
 * the tree. A balloon is not clickable, so nothing crosses back over the ABI
 * and none of the dispatch-safety reasoning the other widgets need applies.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "balloon_api.h"
#include "../chrome/balloon.h"
#include "../loader/zapp_instance.h"
#include "../wm/handle.h"
#include "../wm/wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

struct balloon_rec {
	struct zd_balloon *b;
	struct zd_client *client;
	struct zd_zapp_instance *owner;
	uintptr_t handle;
	bool used;
};

static struct balloon_rec recs[CONFIG_ZD_MAX_BALLOONS];
static uint32_t live_balloons;

static struct balloon_rec *rec_of(struct zd_zapp_instance *owner, uintptr_t handle)
{
	return zd_handle_deref(handle, ZD_HANDLE_BALLOON, owner);
}

/*
 * Released by LVGL, not by us -- the arrangement text_api.c, list_api.c and
 * grid_api.c all use. A balloon dies either because the zapp destroyed it or
 * because the window was reaped and lv_obj_delete() took the subtree; hanging
 * teardown on the object's deletion covers both with one path.
 */
static void view_deleted(lv_event_t *e)
{
	struct balloon_rec *rec = lv_event_get_user_data(e);

	if (rec == NULL || !rec->used) {
		return;
	}

	zd_handle_free(rec->handle);
	rec->used = false;
	rec->b = NULL;
	live_balloons--;
}

uintptr_t zd_balloon_api_create(struct zd_zapp_instance *owner, struct zd_client *client,
				int16_t x, int16_t y, int16_t w, int16_t h, uint32_t icon)
{
	struct balloon_rec *rec = NULL;
	struct zd_balloon *b;

	if (icon > ZD_ICON_WARN) {
		return 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(recs); i++) {
		if (!recs[i].used) {
			rec = &recs[i];
			break;
		}
	}

	if (rec == NULL) {
		LOG_WRN("balloon widget table full (%d)", CONFIG_ZD_MAX_BALLOONS);
		return 0;
	}

	b = zd_balloon_create(client->content, x, y, w, h, icon);
	if (b == NULL) {
		return 0;
	}

	rec->b = b;
	rec->client = client;
	rec->owner = owner;
	rec->used = true;

	rec->handle = zd_handle_alloc(ZD_HANDLE_BALLOON, rec, owner);
	if (rec->handle == 0) {
		rec->used = false;
		zd_balloon_destroy(b);
		return 0;
	}

	live_balloons++;

	lv_obj_add_event_cb(zd_balloon_obj(b), view_deleted, LV_EVENT_DELETE, rec);

	return rec->handle;
}

void zd_balloon_api_destroy(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct balloon_rec *rec = rec_of(owner, handle);

	if (rec != NULL) {
		zd_balloon_destroy(rec->b);
	}
}

int zd_balloon_api_set_text(struct zd_zapp_instance *owner, uintptr_t handle,
			    const char *text)
{
	struct balloon_rec *rec = rec_of(owner, handle);

	if (rec == NULL) {
		return -EINVAL;
	}

	return zd_balloon_set_text(rec->b, text);
}

int zd_balloon_api_set_geometry(struct zd_zapp_instance *owner, uintptr_t handle,
				int16_t x, int16_t y, int16_t w, int16_t h)
{
	struct balloon_rec *rec = rec_of(owner, handle);

	if (rec == NULL) {
		return -EINVAL;
	}

	return zd_balloon_set_geometry(rec->b, x, y, w, h);
}

int zd_balloon_api_set_icon(struct zd_zapp_instance *owner, uintptr_t handle,
			    uint32_t icon, uint32_t state)
{
	struct balloon_rec *rec = rec_of(owner, handle);

	if (rec == NULL) {
		return -EINVAL;
	}

	return zd_balloon_set_icon(rec->b, icon, state);
}

uint32_t zd_balloon_api_live_count(void)
{
	return live_balloons;
}
