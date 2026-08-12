/*
 * zephyr-desktop — implementation of the host API vtable.
 *
 * Every entry point takes a zd_zapp_ctx_t and resolves the caller's instance
 * from it. Nothing here reads a global "current zapp" or "current user": that is
 * what makes multi-user a login screen rather than a refactor, and what would
 * let these become syscalls under CONFIG_USERSPACE without a zapp changing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/llext/symbol.h>
#include <zephyr/logging/log.h>

#include "clipboard.h"
#include "clock.h"
#include "fs_api.h"
#include "host_api.h"
#include "session.h"
#include "list_api.h"
#include "text_api.h"
#include "../chrome/menu.h"
#include "../loader/zapp_instance.h"
#include "../shell/dialog.h"
#include "../wm/client.h"
#include "../wm/handle.h"
#include "../wm/wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/* Resolve the caller. A NULL or corrupt ctx is a zapp bug, not a desktop one. */
static struct zd_zapp_instance *instance_of(zd_zapp_ctx_t ctx)
{
	if (ctx == NULL || ctx->inst == NULL || !ctx->inst->live) {
		return NULL;
	}
	return ctx->inst;
}

static struct zd_client *window_of(zd_zapp_ctx_t ctx, zd_window_t win)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst == NULL) {
		return NULL;
	}

	return zd_handle_deref((uintptr_t)win, ZD_HANDLE_WINDOW, inst);
}

/* --- windows --------------------------------------------------------------- */

static zd_window_t api_window_create(zd_zapp_ctx_t ctx, const struct zd_window_desc *desc)
{
	struct zd_zapp_instance *inst = instance_of(ctx);
	struct zd_client *client;
	lv_area_t geom = { 0 };
	const lv_area_t *want = NULL;
	uintptr_t handle;

	if (inst == NULL || desc == NULL) {
		return NULL;
	}

	if (inst->window_count >= CONFIG_ZD_MAX_WINDOWS_PER_ZAPP) {
		LOG_WRN("'%s' hit its window quota (%d)", inst->name,
			CONFIG_ZD_MAX_WINDOWS_PER_ZAPP);
		return NULL;
	}

	if (desc->geom.w > 0 && desc->geom.h > 0) {
		geom.x1 = desc->geom.x;
		geom.y1 = desc->geom.y;
		geom.x2 = desc->geom.x + desc->geom.w - 1;
		geom.y2 = desc->geom.y + desc->geom.h - 1;
		want = &geom;
	}

	client = zd_wm_window_create(inst->wm, desc->title, want);
	if (client == NULL) {
		return NULL;
	}

	handle = zd_handle_alloc(ZD_HANDLE_WINDOW, client, inst);
	if (handle == 0) {
		zd_wm_window_close(client);
		return NULL;
	}

	client->owner = inst;
	client->handle = handle;
	inst->window_count++;

	/* Only now, with ownership and handle in place, can the zapp be told it
	 * has focus -- the event carries the handle it just received.
	 */
	zd_wm_focus(inst->wm, client);

	return (zd_window_t)handle;
}

static void api_window_close(zd_zapp_ctx_t ctx, zd_window_t win)
{
	struct zd_client *client = window_of(ctx, win);

	if (client != NULL) {
		zd_wm_window_close(client);
	}
}

static int api_window_set_title(zd_zapp_ctx_t ctx, zd_window_t win, const char *title)
{
	struct zd_client *client = window_of(ctx, win);

	if (client == NULL || title == NULL) {
		return -EINVAL;
	}

	zd_wm_window_set_title(client, title);
	return 0;
}

static int api_window_set_geometry(zd_zapp_ctx_t ctx, zd_window_t win,
				   const struct zd_rect *geom)
{
	struct zd_client *client = window_of(ctx, win);

	if (client == NULL || geom == NULL) {
		return -EINVAL;
	}

	return zd_wm_window_set_geometry(client, geom->x, geom->y, geom->w, geom->h);
}

static int api_window_get_geometry(zd_zapp_ctx_t ctx, zd_window_t win, struct zd_rect *out)
{
	struct zd_client *client = window_of(ctx, win);

	if (client == NULL || out == NULL) {
		return -EINVAL;
	}

	out->x = (int16_t)client->geom.x1;
	out->y = (int16_t)client->geom.y1;
	out->w = (int16_t)lv_area_get_width(&client->geom);
	out->h = (int16_t)lv_area_get_height(&client->geom);
	return 0;
}

static void api_window_close_cancel(zd_zapp_ctx_t ctx, zd_window_t win)
{
	struct zd_client *client = window_of(ctx, win);

	if (client != NULL) {
		zd_wm_window_close_cancel(client);
	}
}

static int api_window_minimize(zd_zapp_ctx_t ctx, zd_window_t win)
{
	struct zd_client *client = window_of(ctx, win);

	if (client == NULL) {
		return -EINVAL;
	}

	zd_wm_window_minimize(client);
	return 0;
}

static int api_window_restore(zd_zapp_ctx_t ctx, zd_window_t win)
{
	struct zd_client *client = window_of(ctx, win);

	if (client == NULL) {
		return -EINVAL;
	}

	zd_wm_window_restore(client);
	return 0;
}

/* --- content --------------------------------------------------------------- */

static zd_label_t api_label_create(zd_zapp_ctx_t ctx, zd_window_t win, const char *text,
				   int16_t x, int16_t y)
{
	struct zd_zapp_instance *inst = instance_of(ctx);
	struct zd_client *client = window_of(ctx, win);
	lv_obj_t *label;
	uintptr_t handle;

	if (inst == NULL || client == NULL) {
		return NULL;
	}

	label = lv_label_create(client->content);
	lv_label_set_text(label, text != NULL ? text : "");
	lv_obj_set_pos(label, x, y);

	handle = zd_handle_alloc(ZD_HANDLE_LABEL, label, inst);
	if (handle == 0) {
		lv_obj_delete(label);
		return NULL;
	}

	return (zd_label_t)handle;
}

static int api_label_set_text(zd_zapp_ctx_t ctx, zd_label_t label, const char *text)
{
	struct zd_zapp_instance *inst = instance_of(ctx);
	lv_obj_t *obj;

	if (inst == NULL) {
		return -EINVAL;
	}

	obj = zd_handle_deref((uintptr_t)label, ZD_HANDLE_LABEL, inst);
	if (obj == NULL) {
		return -EINVAL;
	}

	lv_label_set_text(obj, text != NULL ? text : "");
	return 0;
}

static int api_label_set_pos(zd_zapp_ctx_t ctx, zd_label_t label, int16_t x, int16_t y)
{
	struct zd_zapp_instance *inst = instance_of(ctx);
	lv_obj_t *obj;

	if (inst == NULL) {
		return -EINVAL;
	}

	obj = zd_handle_deref((uintptr_t)label, ZD_HANDLE_LABEL, inst);
	if (obj == NULL) {
		return -EINVAL;
	}

	lv_obj_set_pos(obj, x, y);
	return 0;
}

/*
 * Unlike a text widget or a list, a label has no record of its own, so its
 * handle is freed here rather than from an LV_EVENT_DELETE handler. That is not
 * an inconsistency to tidy up: a label owns nothing but its lv_obj_t, and the
 * registry's free_all() at instance teardown already covers the window-close
 * case that DELETE handlers exist to catch for the others.
 */
static void api_label_destroy(zd_zapp_ctx_t ctx, zd_label_t label)
{
	struct zd_zapp_instance *inst = instance_of(ctx);
	lv_obj_t *obj;

	if (inst == NULL) {
		return;
	}

	obj = zd_handle_deref((uintptr_t)label, ZD_HANDLE_LABEL, inst);
	if (obj == NULL) {
		return;
	}

	zd_handle_free((uintptr_t)label);
	lv_obj_delete(obj);
}

/* --- text ------------------------------------------------------------------- */

/*
 * Thin forwarders again, in the shape the storage entry points established:
 * resolve the caller, hand off. Everything interesting -- the handle registry,
 * the byte/character conversion, the suppression of self-inflicted change
 * events -- is in text_api.c, so there is one place to read and one to get
 * wrong.
 */

static zd_text_t api_text_create(zd_zapp_ctx_t ctx, zd_window_t win,
				 const struct zd_rect *geom, uint32_t flags)
{
	struct zd_zapp_instance *inst = instance_of(ctx);
	struct zd_client *client = window_of(ctx, win);

	if (inst == NULL || client == NULL || geom == NULL) {
		return NULL;
	}

	return (zd_text_t)zd_text_create(inst, client, geom, flags);
}

static void api_text_destroy(zd_zapp_ctx_t ctx, zd_text_t text)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst != NULL) {
		zd_text_destroy(inst, (uintptr_t)text);
	}
}

static int api_text_set_text(zd_zapp_ctx_t ctx, zd_text_t text, const char *s)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_set_text(inst, (uintptr_t)text, s) : -EINVAL;
}

static int api_text_get_text(zd_zapp_ctx_t ctx, zd_text_t text, uint32_t from, char *buf,
			     uint32_t len)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_get_text(inst, (uintptr_t)text, from, buf, len)
			    : -EINVAL;
}

static int api_text_get_length(zd_zapp_ctx_t ctx, zd_text_t text)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_get_length(inst, (uintptr_t)text) : -EINVAL;
}

static int api_text_get_capacity(zd_zapp_ctx_t ctx, zd_text_t text)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_get_capacity(inst, (uintptr_t)text) : -EINVAL;
}

static int api_text_insert(zd_zapp_ctx_t ctx, zd_text_t text, const char *s)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_insert(inst, (uintptr_t)text, s) : -EINVAL;
}

static int api_text_set_geometry(zd_zapp_ctx_t ctx, zd_text_t text,
				 const struct zd_rect *geom)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_set_geometry(inst, (uintptr_t)text, geom) : -EINVAL;
}

static int api_text_set_cursor(zd_zapp_ctx_t ctx, zd_text_t text, uint32_t pos)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_set_cursor(inst, (uintptr_t)text, pos) : -EINVAL;
}

static int api_text_get_cursor(zd_zapp_ctx_t ctx, zd_text_t text)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_get_cursor(inst, (uintptr_t)text) : -EINVAL;
}

static int api_text_get_selection(zd_zapp_ctx_t ctx, zd_text_t text, uint32_t *from,
				  uint32_t *to)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_get_selection(inst, (uintptr_t)text, from, to)
			    : -EINVAL;
}

static int api_text_select(zd_zapp_ctx_t ctx, zd_text_t text, uint32_t from, uint32_t to)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_select(inst, (uintptr_t)text, from, to) : -EINVAL;
}

static int api_text_delete_selection(zd_zapp_ctx_t ctx, zd_text_t text)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_delete_selection(inst, (uintptr_t)text) : -EINVAL;
}

/* --- menus -------------------------------------------------------------------- */

static zd_menu_t api_menubar_create(zd_zapp_ctx_t ctx, zd_window_t win)
{
	struct zd_zapp_instance *inst = instance_of(ctx);
	struct zd_client *client = window_of(ctx, win);

	if (inst == NULL || client == NULL) {
		return NULL;
	}

	return (zd_menu_t)zd_menubar_create(inst, client);
}

static zd_menu_t api_menu_add_submenu(zd_zapp_ctx_t ctx, zd_menu_t bar, const char *label)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst == NULL) {
		return NULL;
	}

	return (zd_menu_t)zd_menu_add_submenu(inst, (uintptr_t)bar, label);
}

static int api_menu_add_item(zd_zapp_ctx_t ctx, zd_menu_t menu, const char *label,
			     uint16_t id)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_menu_add_item(inst, (uintptr_t)menu, label, id) : -EINVAL;
}

static int api_menu_add_separator(zd_zapp_ctx_t ctx, zd_menu_t menu)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_menu_add_separator(inst, (uintptr_t)menu) : -EINVAL;
}

static int api_menu_set_item_enabled(zd_zapp_ctx_t ctx, zd_menu_t menu, uint16_t id,
				     bool enabled)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_menu_set_item_enabled(inst, (uintptr_t)menu, id, enabled)
			    : -EINVAL;
}

static int api_window_get_content_size(zd_zapp_ctx_t ctx, zd_window_t win, int16_t *w,
				       int16_t *h)
{
	struct zd_client *client = window_of(ctx, win);

	if (client == NULL || w == NULL || h == NULL) {
		return -EINVAL;
	}

	/* Derived from client->geom, never read back off LVGL: the model is the
	 * truth, and reading back is how ZD_EV_RESIZED came to carry the old
	 * size in milestone J.
	 */
	zd_client_content_size(client, w, h);
	return 0;
}

/* --- dialogs ------------------------------------------------------------------ */

/*
 * A dialog belongs to the instance that asked for it, not to a window: it
 * outlives no zapp, and if the requester dies while one is up the desktop takes
 * it down rather than leaving a modal shade over an unclickable screen. The
 * window is passed only so the answer's event carries a handle the zapp
 * recognises.
 */

static int api_dialog_confirm(zd_zapp_ctx_t ctx, const char *title, const char *msg,
			      uint32_t buttons, uint16_t id)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst == NULL) {
		return -EINVAL;
	}

	return zd_dialog_confirm(inst, inst->wm->focused, title, msg, buttons, id);
}

static int api_dialog_file(zd_zapp_ctx_t ctx, const char *title, enum zd_dir dir,
			   uint32_t mode, uint16_t id)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst == NULL) {
		return -EINVAL;
	}

	return zd_dialog_file(inst, inst->wm->focused, title, dir, mode, id);
}

static int api_dialog_prompt(zd_zapp_ctx_t ctx, const char *title, const char *msg,
			     const char *initial, uint16_t id)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst == NULL) {
		return -EINVAL;
	}

	return zd_dialog_prompt(inst, inst->wm->focused, title, msg, initial, id);
}

static int api_dialog_get_text(zd_zapp_ctx_t ctx, char *buf, uint32_t len)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_dialog_get_text(inst, buf, len) : -EINVAL;
}

static int api_dialog_get_path(zd_zapp_ctx_t ctx, char *buf, uint32_t len)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_dialog_get_path(inst, buf, len) : -EINVAL;
}

static int api_clock_now(zd_zapp_ctx_t ctx, struct zd_time *out)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst == NULL || out == NULL) {
		return -EINVAL;
	}

	zd_clock_now(out);
	return 0;
}

/* --- clipboard --------------------------------------------------------------- */

static int api_clipboard_set(zd_zapp_ctx_t ctx, const char *text, uint32_t len)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_clipboard_set(text, len) : -EINVAL;
}

static int api_clipboard_get(zd_zapp_ctx_t ctx, uint32_t from, char *buf, uint32_t len)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_clipboard_get(from, buf, len) : -EINVAL;
}

static int api_clipboard_length(zd_zapp_ctx_t ctx)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? (int)zd_clipboard_length() : -EINVAL;
}

static int api_text_cut(zd_zapp_ctx_t ctx, zd_text_t text)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_cut(inst, (uintptr_t)text) : -EINVAL;
}

static int api_text_copy(zd_zapp_ctx_t ctx, zd_text_t text)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_copy(inst, (uintptr_t)text) : -EINVAL;
}

static int api_text_paste(zd_zapp_ctx_t ctx, zd_text_t text)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_text_paste(inst, (uintptr_t)text) : -EINVAL;
}

/* --- filesystem and misc ---------------------------------------------------- */

static int api_path_resolve(zd_zapp_ctx_t ctx, enum zd_dir dir, char *out, uint32_t out_len)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst == NULL || out == NULL) {
		return -EINVAL;
	}

	return zd_session_path(inst->session, dir, out, out_len);
}

/*
 * The storage entry points are all the same three lines: resolve the caller,
 * forward. Everything that makes them interesting -- path scoping, quotas, the
 * bus arbiter, the handle registry -- lives in fs_api.c, so that there is one
 * place to read and one place to get wrong.
 */

static int api_fs_open(zd_zapp_ctx_t ctx, const char *path, uint32_t flags, zd_file_t *out)
{
	struct zd_zapp_instance *inst = instance_of(ctx);
	uintptr_t handle;
	int ret;

	if (inst == NULL || out == NULL) {
		return -EINVAL;
	}

	ret = zd_fs_open(inst->session, inst, path, flags, &handle);
	if (ret != 0) {
		return ret;
	}

	*out = (zd_file_t)handle;
	return 0;
}

static int api_fs_read(zd_zapp_ctx_t ctx, zd_file_t file, void *buf, uint32_t len)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_fs_read(inst, (uintptr_t)file, buf, len) : -EINVAL;
}

static int api_fs_write(zd_zapp_ctx_t ctx, zd_file_t file, const void *buf, uint32_t len)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_fs_write(inst, (uintptr_t)file, buf, len) : -EINVAL;
}

static int api_fs_seek(zd_zapp_ctx_t ctx, zd_file_t file, int32_t offset, int whence)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_fs_seek(inst, (uintptr_t)file, offset, whence) : -EINVAL;
}

static int api_fs_tell(zd_zapp_ctx_t ctx, zd_file_t file)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_fs_tell(inst, (uintptr_t)file) : -EINVAL;
}

static int api_fs_sync(zd_zapp_ctx_t ctx, zd_file_t file)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_fs_sync(inst, (uintptr_t)file) : -EINVAL;
}

static void api_fs_close(zd_zapp_ctx_t ctx, zd_file_t file)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst != NULL) {
		zd_fs_close(inst, (uintptr_t)file);
	}
}

static int api_fs_opendir(zd_zapp_ctx_t ctx, const char *path, zd_dir_t *out)
{
	struct zd_zapp_instance *inst = instance_of(ctx);
	uintptr_t handle;
	int ret;

	if (inst == NULL || out == NULL) {
		return -EINVAL;
	}

	ret = zd_fs_opendir(inst->session, inst, path, &handle);
	if (ret != 0) {
		return ret;
	}

	*out = (zd_dir_t)handle;
	return 0;
}

static int api_fs_readdir(zd_zapp_ctx_t ctx, zd_dir_t dir, struct zd_dirent *out)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_fs_readdir(inst, (uintptr_t)dir, out) : -EINVAL;
}

static void api_fs_closedir(zd_zapp_ctx_t ctx, zd_dir_t dir)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst != NULL) {
		zd_fs_closedir(inst, (uintptr_t)dir);
	}
}

static int api_fs_stat(zd_zapp_ctx_t ctx, const char *path, struct zd_dirent *out)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_fs_stat(inst->session, path, out) : -EINVAL;
}

static int api_fs_mkdir(zd_zapp_ctx_t ctx, const char *path)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_fs_mkdir(inst->session, path) : -EINVAL;
}

static int api_fs_unlink(zd_zapp_ctx_t ctx, const char *path)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_fs_unlink(inst->session, path) : -EINVAL;
}

static int api_fs_rename(zd_zapp_ctx_t ctx, const char *from, const char *to)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_fs_rename(inst->session, from, to) : -EINVAL;
}

static void api_log(zd_zapp_ctx_t ctx, int level, const char *msg)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	ARG_UNUSED(level);

	if (inst == NULL || msg == NULL) {
		return;
	}

	LOG_INF("[%s] %s", inst->name, msg);
}

static int64_t api_uptime_ms(void)
{
	return k_uptime_get();
}

static void api_set_user_data(zd_zapp_ctx_t ctx, void *data)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst != NULL) {
		inst->user_data = data;
	}
}

static void *api_get_user_data(zd_zapp_ctx_t ctx)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? inst->user_data : NULL;
}

static void *api_unsafe_lvgl_content(zd_zapp_ctx_t ctx, zd_window_t win)
{
	struct zd_client *client = window_of(ctx, win);

	return client != NULL ? client->content : NULL;
}

/* --- lists ------------------------------------------------------------------- */

/*
 * Thin forwarders, same as the text ones and for the same reason. The handle
 * check happens inside list_api.c; what happens here is only the ctx -> owner
 * and win -> client resolution that every entry point needs.
 */

static zd_list_t api_list_create(zd_zapp_ctx_t ctx, zd_window_t win,
				 const struct zd_rect *geom, uint32_t flags)
{
	struct zd_zapp_instance *inst = instance_of(ctx);
	struct zd_client *client = window_of(ctx, win);

	if (inst == NULL || client == NULL || geom == NULL) {
		return NULL;
	}

	return (zd_list_t)zd_list_create(inst, client, geom, flags);
}

static void api_list_destroy(zd_zapp_ctx_t ctx, zd_list_t list)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst != NULL) {
		zd_list_destroy(inst, (uintptr_t)list);
	}
}

static int api_list_set_geometry(zd_zapp_ctx_t ctx, zd_list_t list,
				 const struct zd_rect *geom)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_list_set_geometry(inst, (uintptr_t)list, geom) : -EINVAL;
}

static int api_list_clear(zd_zapp_ctx_t ctx, zd_list_t list)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_list_clear(inst, (uintptr_t)list) : -EINVAL;
}

static int api_list_add_item(zd_zapp_ctx_t ctx, zd_list_t list, const char *text,
			     uint16_t id)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_list_add_item(inst, (uintptr_t)list, text, id) : -EINVAL;
}

static int api_list_get_count(zd_zapp_ctx_t ctx, zd_list_t list)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_list_get_count(inst, (uintptr_t)list) : -EINVAL;
}

static int api_list_get_capacity(zd_zapp_ctx_t ctx, zd_list_t list)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_list_get_capacity(inst, (uintptr_t)list) : -EINVAL;
}

static int api_list_get_selected(zd_zapp_ctx_t ctx, zd_list_t list)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_list_get_selected(inst, (uintptr_t)list) : -EINVAL;
}

static int api_list_set_selected(zd_zapp_ctx_t ctx, zd_list_t list, int32_t index)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_list_set_selected(inst, (uintptr_t)list, index) : -EINVAL;
}

static int api_list_get_item_id(zd_zapp_ctx_t ctx, zd_list_t list, int32_t index)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_list_get_item_id(inst, (uintptr_t)list, index) : -EINVAL;
}

static int api_list_get_item_text(zd_zapp_ctx_t ctx, zd_list_t list, int32_t index,
				  char *buf, uint32_t len)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	return inst != NULL ? zd_list_get_item_text(inst, (uintptr_t)list, index, buf, len)
			    : -EINVAL;
}

/* --- the tables ------------------------------------------------------------- */

#define ZD_HOST_API_COMMON                                                                 \
	.abi_major = ZD_ABI_MAJOR, .abi_minor = ZD_ABI_MINOR,                              \
	.struct_size = sizeof(struct zd_host_api), .window_create = api_window_create,     \
	.window_close = api_window_close, .window_set_title = api_window_set_title,        \
	.window_set_geometry = api_window_set_geometry,                                    \
	.window_get_geometry = api_window_get_geometry, .label_create = api_label_create,  \
	.label_set_text = api_label_set_text, .path_resolve = api_path_resolve,            \
	.log = api_log, .uptime_ms = api_uptime_ms, .set_user_data = api_set_user_data,      \
	.get_user_data = api_get_user_data, .fs_open = api_fs_open, .fs_read = api_fs_read, \
	.fs_write = api_fs_write, .fs_seek = api_fs_seek, .fs_tell = api_fs_tell,           \
	.fs_sync = api_fs_sync, .fs_close = api_fs_close, .fs_opendir = api_fs_opendir,     \
	.fs_readdir = api_fs_readdir, .fs_closedir = api_fs_closedir,                       \
	.fs_stat = api_fs_stat, .fs_mkdir = api_fs_mkdir, .fs_unlink = api_fs_unlink,       \
	.fs_rename = api_fs_rename, .window_minimize = api_window_minimize,                 \
	.window_restore = api_window_restore, .text_create = api_text_create,               \
	.text_destroy = api_text_destroy, .text_set_text = api_text_set_text,               \
	.text_get_text = api_text_get_text, .text_get_length = api_text_get_length,                                             \
	.text_get_capacity = api_text_get_capacity,         \
	.text_insert = api_text_insert, .text_set_geometry = api_text_set_geometry,         \
	.text_set_cursor = api_text_set_cursor, .text_get_cursor = api_text_get_cursor,     \
	.text_get_selection = api_text_get_selection, .text_select = api_text_select,       \
	.text_delete_selection = api_text_delete_selection,                                 \
	.clipboard_set = api_clipboard_set, .clipboard_get = api_clipboard_get,             \
	.clipboard_length = api_clipboard_length, .text_cut = api_text_cut,                 \
	.text_copy = api_text_copy, .text_paste = api_text_paste,                           \
	.menubar_create = api_menubar_create,                                               \
	.menu_add_submenu = api_menu_add_submenu, .menu_add_item = api_menu_add_item,       \
	.menu_add_separator = api_menu_add_separator,                                       \
	.menu_set_item_enabled = api_menu_set_item_enabled,                                 \
	.window_get_content_size = api_window_get_content_size,                             \
	.dialog_confirm = api_dialog_confirm, .dialog_file = api_dialog_file,               \
	.dialog_get_path = api_dialog_get_path, .clock_now = api_clock_now,                \
	.window_close_cancel = api_window_close_cancel,                                     \
	.list_create = api_list_create, .list_destroy = api_list_destroy,                   \
	.list_set_geometry = api_list_set_geometry, .list_clear = api_list_clear,           \
	.list_add_item = api_list_add_item, .list_get_count = api_list_get_count,           \
	.list_get_capacity = api_list_get_capacity,                                         \
	.list_get_selected = api_list_get_selected,                                         \
	.list_set_selected = api_list_set_selected,                                         \
	.list_get_item_id = api_list_get_item_id,                                           \
	.list_get_item_text = api_list_get_item_text,                                       \
	.label_set_pos = api_label_set_pos, .label_destroy = api_label_destroy,             \
	.dialog_prompt = api_dialog_prompt, .dialog_get_text = api_dialog_get_text

static const struct zd_host_api host_api_untrusted = {
	ZD_HOST_API_COMMON,
	.unsafe_lvgl_content = NULL,
};

static const struct zd_host_api host_api_trusted = {
	ZD_HOST_API_COMMON,
	.unsafe_lvgl_content = api_unsafe_lvgl_content,
};

const struct zd_host_api *zd_host_api_for(struct zd_zapp_instance *inst)
{
	bool trusted = inst->manifest != NULL &&
		       (inst->manifest->flags & ZD_ZAPP_FLAG_TRUSTED) != 0;

	return trusted ? &host_api_trusted : &host_api_untrusted;
}

/*
 * The single symbol the desktop exports to extensions. Everything else a zapp
 * can reach, it reaches through the returned table.
 */
const struct zd_host_api *zd_get_host_api(void)
{
	return &host_api_untrusted;
}
EXPORT_GROUP_SYMBOL(DESKTOP, zd_get_host_api);
