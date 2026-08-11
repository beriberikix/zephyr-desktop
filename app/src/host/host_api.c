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

#include "host_api.h"
#include "session.h"
#include "../loader/zapp_instance.h"
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

/* --- filesystem and misc ---------------------------------------------------- */

static int api_path_resolve(zd_zapp_ctx_t ctx, enum zd_dir dir, char *out, uint32_t out_len)
{
	struct zd_zapp_instance *inst = instance_of(ctx);

	if (inst == NULL || out == NULL) {
		return -EINVAL;
	}

	return zd_session_path(inst->session, dir, out, out_len);
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

/* --- the tables ------------------------------------------------------------- */

#define ZD_HOST_API_COMMON                                                                 \
	.abi_major = ZD_ABI_MAJOR, .abi_minor = ZD_ABI_MINOR,                              \
	.struct_size = sizeof(struct zd_host_api), .window_create = api_window_create,     \
	.window_close = api_window_close, .window_set_title = api_window_set_title,        \
	.window_set_geometry = api_window_set_geometry,                                    \
	.window_get_geometry = api_window_get_geometry, .label_create = api_label_create,  \
	.label_set_text = api_label_set_text, .path_resolve = api_path_resolve,            \
	.log = api_log, .uptime_ms = api_uptime_ms, .set_user_data = api_set_user_data,      \
	.get_user_data = api_get_user_data

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
