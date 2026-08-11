/*
 * zephyr-desktop — the app lifecycle.
 *
 * discover -> load -> bringup -> bind -> init -> run -> teardown -> unload.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/llext/fs_loader.h>
#include <zephyr/llext/llext.h>
#include <zephyr/logging/log.h>

#include "app_instance.h"
#include "../host/host_api.h"
#include "../wm/handle.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

#define MAX_INSTANCES CONFIG_ZD_MAX_CLIENTS

struct instance_slot {
	struct zd_app_instance inst;
	struct llext_fs_loader fs_loader;
};

static struct instance_slot slots[MAX_INSTANCES];
static struct zd_wm *loader_wm;
static struct zd_session *loader_session;
static uint32_t next_instance_id = 1;
static uint32_t live_instances;

void zd_app_loader_init(struct zd_wm *wm, struct zd_session *session)
{
	loader_wm = wm;
	loader_session = session;
}

static struct instance_slot *alloc_slot(void)
{
	for (size_t i = 0; i < MAX_INSTANCES; i++) {
		if (!slots[i].inst.live) {
			return &slots[i];
		}
	}
	return NULL;
}

static int validate_manifest(const struct zd_app_manifest *manifest, const char *path)
{
	if (manifest->magic != ZD_APP_MAGIC) {
		LOG_ERR("%s: bad manifest magic 0x%08x", path, manifest->magic);
		return -EINVAL;
	}

	/* Major must match exactly; a newer minor on the app side means it was
	 * built against a host that had fields we do not.
	 */
	if (manifest->abi_major != ZD_ABI_MAJOR) {
		LOG_ERR("%s: ABI major %u, desktop speaks %u", path, manifest->abi_major,
			ZD_ABI_MAJOR);
		return -ENOTSUP;
	}

	if (manifest->abi_minor > ZD_ABI_MINOR) {
		LOG_ERR("%s: ABI minor %u newer than desktop's %u", path,
			manifest->abi_minor, ZD_ABI_MINOR);
		return -ENOTSUP;
	}

	if (manifest->init == NULL) {
		LOG_ERR("%s: manifest has no init", path);
		return -EINVAL;
	}

	return 0;
}

/* Drop this instance's reference to the image, running .fini_array only if we
 * are the last holder. Shared with the failure paths in zd_app_launch().
 */
static void unwind_image(struct zd_app_instance *inst)
{
	if (inst->ext == NULL) {
		return;
	}

	if (inst->ext->use_count == 1) {
		llext_teardown(inst->ext);
	}

	llext_unload(&inst->ext);
	inst->ext = NULL;
}

int zd_app_launch(const struct zd_app_entry *entry)
{
	struct instance_slot *slot;
	struct zd_app_instance *inst;
	const struct zd_app_manifest *manifest;
	const void *sym;
	int ret;

	slot = alloc_slot();
	if (slot == NULL) {
		LOG_ERR("no free instance slots; cannot launch '%s'", entry->name);
		return -ENOMEM;
	}

	inst = &slot->inst;
	memset(inst, 0, sizeof(*inst));
	slot->fs_loader = (struct llext_fs_loader)LLEXT_FS_LOADER(entry->path);

	inst->loader = &slot->fs_loader.loader;
	inst->session = loader_session;
	inst->wm = loader_wm;
	inst->ctx.inst = inst;
	inst->id = next_instance_id++;
	strncpy(inst->name, entry->name, sizeof(inst->name) - 1);

	struct llext_load_param param = LLEXT_LOAD_PARAM_DEFAULT;

	/* llext refcounts by name. A negative return is a real failure; zero
	 * means we loaded it; a positive value is the PREVIOUS use count, i.e.
	 * this image was already resident and we are now sharing it. Treating
	 * "already loaded" as an error is an easy and silent mistake -- it makes
	 * the second instance of any app fail to start.
	 */
	ret = llext_load(inst->loader, entry->name, &inst->ext, &param);
	if (ret < 0) {
		LOG_ERR("llext_load('%s') failed (%d)", entry->path, ret);
		return ret;
	}

	inst->owns_image = (ret == 0);

	if (inst->owns_image) {
		/* .init_array runs once per image, not once per instance. */
		ret = llext_bringup(inst->ext);
		if (ret != 0) {
			LOG_ERR("llext_bringup('%s') failed (%d)", entry->name, ret);
			llext_unload(&inst->ext);
			return ret;
		}
	} else {
		LOG_DBG("'%s' image already resident; sharing it", entry->name);
	}

	sym = llext_find_sym(&inst->ext->exp_tab, ZD_APP_MANIFEST_SYM);
	if (sym == NULL) {
		LOG_ERR("'%s' exports no %s", entry->name, ZD_APP_MANIFEST_SYM);
		unwind_image(inst);
		return -ENOEXEC;
	}

	manifest = sym;
	ret = validate_manifest(manifest, entry->path);
	if (ret != 0) {
		unwind_image(inst);
		return ret;
	}

	inst->manifest = manifest;
	inst->live = true;
	live_instances++;

	if (manifest->name != NULL) {
		strncpy(inst->name, manifest->name, sizeof(inst->name) - 1);
		inst->name[sizeof(inst->name) - 1] = '\0';
	}

	LOG_INF("launched '%s' instance %u (ABI %u.%u)", inst->name, inst->id,
		manifest->abi_major, manifest->abi_minor);

	/* Everything the app does from here can call back into the host API, so
	 * the guard has to be held across init exactly as across an event.
	 */
	inst->initialising = true;
	loader_wm->in_app_callback++;
	ret = manifest->init(&inst->ctx, zd_host_api_for(inst));
	loader_wm->in_app_callback--;
	inst->initialising = false;

	if (ret != 0) {
		LOG_ERR("'%s' init failed (%d); unloading", inst->name, ret);
		zd_app_request_unload(inst);
		return ret;
	}

	/* Creating a window focuses it, which would otherwise deliver
	 * ZD_EV_WINDOW_FOCUS while init() is still running -- before the app has
	 * had a chance to record the handle it was just given, so it cannot yet
	 * recognise its own window. Events are suppressed during init and the
	 * focus state is re-asserted here, once the app is fully constructed.
	 */
	if (loader_wm->focused != NULL && loader_wm->focused->owner == inst) {
		struct zd_event ev = {
			.type = ZD_EV_WINDOW_FOCUS,
			.win = (zd_window_t)loader_wm->focused->handle,
		};

		zd_app_dispatch(inst, &ev);
	}

	return 0;
}

void zd_app_dispatch(struct zd_app_instance *inst, const struct zd_event *ev)
{
	if (!inst->live || inst->initialising || inst->pending_unload ||
	    inst->manifest->event == NULL) {
		return;
	}

	inst->wm->in_app_callback++;
	inst->manifest->event(&inst->ctx, ev);
	inst->wm->in_app_callback--;
}

void zd_app_request_unload(struct zd_app_instance *inst)
{
	if (!inst->live || inst->pending_unload) {
		return;
	}

	inst->pending_unload = true;
	LOG_DBG("instance %u '%s' queued for unload", inst->id, inst->name);
}

void zd_app_window_gone(struct zd_app_instance *inst)
{
	if (inst == NULL || !inst->live) {
		return;
	}

	if (inst->window_count > 0) {
		inst->window_count--;
	}

	/* An app with no windows left has nothing to interact with. Closing the
	 * last window is how you quit.
	 */
	if (inst->window_count == 0) {
		zd_app_request_unload(inst);
	}
}

void zd_app_on_client_destroyed(struct zd_client *client)
{
	struct zd_app_instance *inst = client->owner;

	if (client->handle != 0) {
		zd_handle_free(client->handle);
		client->handle = 0;
	}

	zd_app_window_gone(inst);
}

void zd_app_on_client_focus(struct zd_client *client, bool focused)
{
	struct zd_app_instance *inst = client->owner;
	struct zd_event ev = {
		.type = focused ? ZD_EV_WINDOW_FOCUS : ZD_EV_WINDOW_BLUR,
		.win = (zd_window_t)client->handle,
	};

	if (inst == NULL || client->handle == 0) {
		return; /* desktop-internal window */
	}

	zd_app_dispatch(inst, &ev);
}

static void finish_unload(struct zd_app_instance *inst)
{
	LOG_DBG("unloading instance %u '%s'", inst->id, inst->name);

	if (inst->manifest != NULL && inst->manifest->fini != NULL) {
		inst->wm->in_app_callback++;
		inst->manifest->fini(&inst->ctx);
		inst->wm->in_app_callback--;
	}

	/* Any handle the app still holds dies with it; the generation bump means
	 * a stale copy can never resolve, even into a later instance's slot.
	 */
	zd_handle_free_all(inst);

	/* Only the last instance sharing this image tears it down. */
	unwind_image(inst);

	inst->manifest = NULL;
	inst->live = false;
	inst->pending_unload = false;
	live_instances--;

	LOG_INF("unloaded instance %u; %u app(s) live, %u handle(s) live", inst->id,
		live_instances, zd_handle_live_count());
}

void zd_app_reap(void)
{
	/* Same rule as the window reap, for the same reason -- only more so.
	 * llext_unload() frees the very text an app frame would return into.
	 */
	if (loader_wm == NULL || loader_wm->in_app_callback > 0) {
		return;
	}

	for (size_t i = 0; i < MAX_INSTANCES; i++) {
		struct zd_app_instance *inst = &slots[i].inst;

		if (!inst->live || !inst->pending_unload) {
			continue;
		}

		/* Windows first: the app's LVGL objects must be gone before its
		 * code is. The WM reap runs before this one in the loop.
		 */
		if (inst->window_count > 0) {
			continue;
		}

		finish_unload(inst);
	}
}

uint32_t zd_app_instance_count(void)
{
	return live_instances;
}
