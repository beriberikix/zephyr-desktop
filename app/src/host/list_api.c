/*
 * zephyr-desktop — implementation of the zapp-facing list widget.
 *
 * Thin, deliberately. Everything hard about a list -- the deferred rebuild, the
 * shared row pool, the double-click ordering trap -- is in chrome/rowlist.c,
 * where the file picker can have it too. What is left here is the part only a
 * zapp needs: a generation-counted handle with an owner check, and a way for a
 * row click to become an event on the other side of the ABI.
 *
 * One thing is worth saying out loud. zd_list_clear() and zd_list_add_item()
 * are routinely called from inside the dispatch of ZD_EV_LIST_ACTIVATE -- the
 * zapp is refilling the very list whose row was just clicked -- and that is
 * supported rather than tolerated. It works because those calls write a model
 * and nothing else; see rowlist.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "list_api.h"
#include "text_api.h"
#include "../chrome/rowlist.h"
#include "../loader/zapp_instance.h"
#include "../wm/handle.h"
#include "../wm/wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

struct list_rec {
	struct zd_rowlist *rl;
	struct zd_client *client;
	struct zd_zapp_instance *owner;
	uintptr_t handle;
	bool used;
};

static struct list_rec recs[CONFIG_ZD_MAX_LISTS];
static uint32_t live_lists;

static struct list_rec *rec_of(struct zd_zapp_instance *owner, uintptr_t handle)
{
	return zd_handle_deref(handle, ZD_HANDLE_LIST, owner);
}

/* --- events out ---------------------------------------------------------------- */

static void emit(struct list_rec *rec, enum zd_event_type type, int32_t index,
		 uint16_t id)
{
	struct zd_event ev = {
		.type = type,
		.win = (zd_window_t)rec->client->handle,
		.list = {
			.list = (zd_list_t)rec->handle,
			.index = (int16_t)index,
			.id = id,
		},
	};

	if (rec->owner == NULL || rec->client->handle == 0) {
		return;
	}

	zd_zapp_dispatch(rec->owner, &ev);
}

static void on_select(void *user, int32_t index, uint16_t id)
{
	struct list_rec *rec = user;

	/* Clicking a row gives the list the keyboard, so the arrow keys carry
	 * on from where the pointer left off rather than from wherever the
	 * selection happened to be.
	 */
	zd_list_focus(rec->client, zd_rowlist_obj(rec->rl));

	emit(rec, ZD_EV_LIST_SELECT, index, id);
}

static void on_activate(void *user, int32_t index, uint16_t id)
{
	emit(user, ZD_EV_LIST_ACTIVATE, index, id);
}

/* --- focus within a window ------------------------------------------------------ */

/*
 * A window has one place keys go, and this is half of deciding which.
 *
 * The WM asks the text hook first and this one second, and neither knows the
 * other exists -- what keeps them from both claiming the keyboard is that each
 * clears the other's focus field here. See wm/keys.c for the order and why it
 * is that way round.
 */
void zd_list_focus(struct zd_client *client, lv_obj_t *view)
{
	if (client == NULL || client->list_focus == view) {
		return;
	}

	client->list_focus = view;

	if (view != NULL) {
		/* The caret must go: two widgets in one window both acting on
		 * Up would be a coin toss from the user's side. Deliberately
		 * NOT raising the on-screen keyboard, unlike text focus --
		 * there is nothing to type into a list.
		 */
		zd_text_focus(client, NULL);
	}
}

bool zd_list_on_client_key(struct zd_client *client, uint32_t code, uint32_t unicode,
			   uint16_t mods)
{
	ARG_UNUSED(unicode);

	if (client->list_focus == NULL) {
		return false;
	}

	for (size_t i = 0; i < ARRAY_SIZE(recs); i++) {
		if (recs[i].used && zd_rowlist_obj(recs[i].rl) == client->list_focus) {
			return zd_rowlist_key(recs[i].rl, code, mods);
		}
	}

	return false;
}

/* --- lifecycle ------------------------------------------------------------------ */

/*
 * Released by LVGL, not by us -- the same arrangement text_api.c uses. A list
 * dies either because the zapp destroyed it or because the window was reaped
 * and lv_obj_delete() took the subtree; hanging teardown on the container's
 * deletion covers both with one path.
 *
 * The rowlist frees its own rows on the same event. Ordering between the two
 * DELETE callbacks does not matter, because neither touches the other's state.
 */
static void view_deleted(lv_event_t *e)
{
	struct list_rec *rec = lv_event_get_user_data(e);

	if (!rec->used) {
		return;
	}

	if (rec->client != NULL && rec->client->list_focus == zd_rowlist_obj(rec->rl)) {
		rec->client->list_focus = NULL;
	}

	zd_handle_free(rec->handle);
	rec->used = false;
	rec->rl = NULL;
	live_lists--;
}

uintptr_t zd_list_create(struct zd_zapp_instance *owner, struct zd_client *client,
			 const struct zd_rect *geom, uint32_t flags)
{
	struct list_rec *rec = NULL;
	struct zd_rowlist *rl;
	lv_obj_t *view;

	ARG_UNUSED(flags); /* none defined in 0.6; the parameter is for later */

	if (geom == NULL) {
		return 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(recs); i++) {
		if (!recs[i].used) {
			rec = &recs[i];
			break;
		}
	}

	if (rec == NULL) {
		LOG_WRN("list widget table full (%d)", CONFIG_ZD_MAX_LISTS);
		return 0;
	}

	rl = zd_rowlist_create(client->content, geom->x, geom->y, geom->w, geom->h);
	if (rl == NULL) {
		return 0;
	}

	view = zd_rowlist_obj(rl);

	rec->rl = rl;
	rec->client = client;
	rec->owner = owner;
	rec->used = true;

	rec->handle = zd_handle_alloc(ZD_HANDLE_LIST, rec, owner);
	if (rec->handle == 0) {
		rec->used = false;
		zd_rowlist_destroy(rl);
		return 0;
	}

	live_lists++;

	zd_rowlist_set_cb(rl, on_select, on_activate, rec);
	lv_obj_add_event_cb(view, view_deleted, LV_EVENT_DELETE, rec);

	/* Unlike a text widget, a new list does NOT take the keyboard. A text
	 * widget is where typing obviously goes; a list only wants the arrows,
	 * and a window with a list and a text field would otherwise depend on
	 * which was created first.
	 */

	return rec->handle;
}

void zd_list_destroy(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct list_rec *rec = rec_of(owner, handle);

	if (rec != NULL) {
		zd_rowlist_destroy(rec->rl);
	}
}

int zd_list_set_geometry(struct zd_zapp_instance *owner, uintptr_t handle,
			 const struct zd_rect *geom)
{
	struct list_rec *rec = rec_of(owner, handle);

	if (rec == NULL || geom == NULL) {
		return -EINVAL;
	}

	zd_rowlist_set_geometry(rec->rl, geom->x, geom->y, geom->w, geom->h);
	return 0;
}

/* --- contents -------------------------------------------------------------------- */

int zd_list_clear(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct list_rec *rec = rec_of(owner, handle);

	if (rec == NULL) {
		return -EINVAL;
	}

	zd_rowlist_clear(rec->rl);
	return 0;
}

int zd_list_add_item(struct zd_zapp_instance *owner, uintptr_t handle, const char *text,
		     uint16_t id)
{
	struct list_rec *rec = rec_of(owner, handle);

	if (rec == NULL || text == NULL) {
		return -EINVAL;
	}

	return zd_rowlist_add(rec->rl, text, id);
}

int zd_list_get_count(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct list_rec *rec = rec_of(owner, handle);

	return rec != NULL ? zd_rowlist_count(rec->rl) : -EINVAL;
}

int zd_list_get_capacity(struct zd_zapp_instance *owner, uintptr_t handle)
{
	return rec_of(owner, handle) != NULL ? zd_rowlist_capacity() : -EINVAL;
}

int zd_list_get_selected(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct list_rec *rec = rec_of(owner, handle);

	return rec != NULL ? zd_rowlist_selected(rec->rl) : -EINVAL;
}

int zd_list_set_selected(struct zd_zapp_instance *owner, uintptr_t handle, int32_t index)
{
	struct list_rec *rec = rec_of(owner, handle);

	return rec != NULL ? zd_rowlist_select(rec->rl, index) : -EINVAL;
}

int zd_list_get_item_id(struct zd_zapp_instance *owner, uintptr_t handle, int32_t index)
{
	struct list_rec *rec = rec_of(owner, handle);

	return rec != NULL ? zd_rowlist_item_id(rec->rl, index) : -EINVAL;
}

int zd_list_get_item_text(struct zd_zapp_instance *owner, uintptr_t handle, int32_t index,
			  char *buf, uint32_t len)
{
	struct list_rec *rec = rec_of(owner, handle);

	return rec != NULL ? zd_rowlist_item_text(rec->rl, index, buf, len) : -EINVAL;
}

uint32_t zd_list_live_count(void)
{
	return live_lists;
}
