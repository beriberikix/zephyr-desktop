/*
 * zephyr-desktop — the zapp lifecycle.
 *
 * discover -> load -> bringup -> bind -> init -> run -> teardown -> unload.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/llext/fs_loader.h>
#include <zephyr/llext/llext.h>
#include <zephyr/logging/log.h>

#include "zapp_instance.h"
#include "../host/bus_arb.h"
#include "../host/fs_api.h"
#include "../host/host_api.h"
#include "../shell/dialog.h"
#include "../wm/handle.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

#define MAX_INSTANCES CONFIG_ZD_MAX_CLIENTS

struct instance_slot {
	struct zd_zapp_instance inst;
	union {
		struct llext_fs_loader fs;
		struct llext_buf_loader buf;
	} loader;
};

static struct instance_slot slots[MAX_INSTANCES];
static struct zd_wm *loader_wm;
static struct zd_session *loader_session;
static uint32_t next_instance_id = 1;
static uint32_t live_instances;

void zd_zapp_loader_init(struct zd_wm *wm, struct zd_session *session)
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

static int validate_manifest(const struct zd_zapp_manifest *manifest, const char *path)
{
	if (manifest->magic != ZD_ZAPP_MAGIC) {
		LOG_ERR("%s: bad manifest magic 0x%08x", path, manifest->magic);
		return -EINVAL;
	}

	/* Major must match exactly; a newer minor on the zapp side means it was
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

#ifdef CONFIG_ZD_ZAPP_LOAD_VIA_BUFFER
/*
 * Slurp the whole ELF into RAM so a peek()-capable loader can be used.
 *
 * llext may modify this buffer in place while linking, and requires it to stay
 * allocated for as long as the zapp is loaded -- so it is owned by the instance
 * and freed in unwind_image(), not here.
 */
static int read_file(const char *path, void **out, size_t *out_len)
{
	struct fs_dirent entry;
	struct fs_file_t file;
	void *buf;
	ssize_t got;
	int ret;

	ret = fs_stat(path, &entry);
	if (ret != 0) {
		return ret;
	}

	buf = k_malloc(entry.size);
	if (buf == NULL) {
		LOG_ERR("no room for a %zu-byte zapp image", entry.size);
		return -ENOMEM;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_READ);
	if (ret != 0) {
		k_free(buf);
		return ret;
	}

	got = fs_read(&file, buf, entry.size);
	fs_close(&file);

	if (got < 0 || (size_t)got != entry.size) {
		LOG_ERR("short read on %s (%zd of %zu)", path, got, entry.size);
		k_free(buf);
		return -EIO;
	}

	*out = buf;
	*out_len = entry.size;
	return 0;
}

static int read_whole_file(const char *path, void **out, size_t *out_len)
{
	int ret;

	zd_bus_storage_acquire();
	ret = read_file(path, out, out_len);
	zd_bus_storage_release();

	return ret;
}
#endif /* CONFIG_ZD_ZAPP_LOAD_VIA_BUFFER */

/* Point the instance at whichever loader this target can actually use. */
static int open_loader(struct instance_slot *slot, const struct zd_zapp_entry *entry)
{
#ifdef CONFIG_ZD_ZAPP_LOAD_VIA_BUFFER
	struct zd_zapp_instance *inst = &slot->inst;
	int ret = read_whole_file(entry->path, &inst->elf_buf, &inst->elf_size);

	if (ret != 0) {
		return ret;
	}

	/* LLEXT_BUF_LOADER picks WRITABLE or PERSISTENT storage from
	 * CONFIG_LLEXT_STORAGE_WRITABLE, which is exactly the distinction that
	 * forced this path in the first place.
	 */
	slot->loader.buf = (struct llext_buf_loader)
		LLEXT_BUF_LOADER(inst->elf_buf, inst->elf_size);
	inst->loader = &slot->loader.buf.loader;
	return 0;
#else
	slot->loader.fs = (struct llext_fs_loader)LLEXT_FS_LOADER(entry->path);
	slot->inst.loader = &slot->loader.fs.loader;
	return 0;
#endif
}

/* Drop this instance's reference to the image, running .fini_array only if we
 * are the last holder. Shared with the failure paths in zd_zapp_launch().
 */
static void unwind_image(struct zd_zapp_instance *inst)
{
	if (inst->ext == NULL) {
		return;
	}

	if (inst->ext->use_count == 1) {
		llext_teardown(inst->ext);
	}

	llext_unload(&inst->ext);
	inst->ext = NULL;

	/* Safe only now: llext may have been referencing straight into this
	 * buffer for the whole life of the zapp.
	 */
	if (inst->elf_buf != NULL) {
		k_free(inst->elf_buf);
		inst->elf_buf = NULL;
		inst->elf_size = 0;
	}
}

/*
 * Is this image already running, and does it want to be the only one?
 *
 * ZD_ZAPP_FLAG_SINGLETON was defined in 0.1 and enforced nowhere until now,
 * which was harmless while the Start menu was the only way to launch anything
 * -- a user who clicks a zapp twice has asked for it twice. A file browser that
 * launches a zapp per double-click is what makes the flag mean something.
 *
 * Matched on the manifest pointer rather than the name, and that is not a
 * shortcut: llext refcounts by name, so two instances of one zapp are looking
 * at the same .data. Same manifest is the same image, exactly.
 */
static struct zd_zapp_instance *singleton_already_running(
	const struct zd_zapp_manifest *manifest)
{
	if ((manifest->flags & ZD_ZAPP_FLAG_SINGLETON) == 0) {
		return NULL;
	}

	for (size_t i = 0; i < MAX_INSTANCES; i++) {
		struct zd_zapp_instance *other = &slots[i].inst;

		if (other->live && !other->pending_unload && other->manifest == manifest) {
			return other;
		}
	}

	return NULL;
}

static void set_launch_arg(struct zd_zapp_instance *inst, const char *arg)
{
	if (arg == NULL) {
		inst->launch_arg[0] = '\0';
		return;
	}

	strncpy(inst->launch_arg, arg, sizeof(inst->launch_arg) - 1);
	inst->launch_arg[sizeof(inst->launch_arg) - 1] = '\0';
}

int zd_zapp_get_launch_arg(struct zd_zapp_instance *inst, char *buf, uint32_t len)
{
	size_t need;

	if (inst == NULL || buf == NULL || len == 0) {
		return -EINVAL;
	}

	need = strlen(inst->launch_arg);
	if (need + 1 > len) {
		/* Never half a path: the caller would open the wrong thing, or
		 * nothing, and could not tell which.
		 */
		return -ENOSPC;
	}

	memcpy(buf, inst->launch_arg, need + 1);
	return (int)need;
}

/** Raise a running singleton's topmost window and tell it what was asked for. */
static void relaunch(struct zd_zapp_instance *inst, const char *arg)
{
	struct zd_client *client;
	struct zd_event ev = { .type = ZD_EV_LAUNCH_ARG };

	set_launch_arg(inst, arg);

	client = zd_wm_topmost_of(loader_wm, inst);
	if (client != NULL) {
		zd_wm_raise(loader_wm, client);
		zd_wm_focus(loader_wm, client);
		ev.win = (zd_window_t)client->handle;
	}

	LOG_INF("'%s' is already running; raising it", inst->name);
	zd_zapp_dispatch(inst, &ev);
}

int zd_zapp_launch(const struct zd_zapp_entry *entry, const char *arg)
{
	struct instance_slot *slot;
	struct zd_zapp_instance *inst;
	const struct zd_zapp_manifest *manifest;
	const void *sym;
	int ret;

	slot = alloc_slot();
	if (slot == NULL) {
		LOG_ERR("no free instance slots; cannot launch '%s'", entry->name);
		return -ENOMEM;
	}

	inst = &slot->inst;
	memset(inst, 0, sizeof(*inst));

	ret = open_loader(slot, entry);
	if (ret != 0) {
		LOG_ERR("cannot open '%s' (%d)", entry->path, ret);
		return ret;
	}

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
	 * the second instance of any zapp fail to start.
	 */
	ret = llext_load(inst->loader, entry->name, &inst->ext, &param);
	if (ret < 0) {
		LOG_ERR("llext_load('%s') failed (%d)", entry->path, ret);
		k_free(inst->elf_buf);
		inst->elf_buf = NULL;
		return ret;
	}

	inst->owns_image = (ret == 0);

	if (inst->owns_image) {
		/* .init_array runs once per image, not once per instance. */
		ret = llext_bringup(inst->ext);
		if (ret != 0) {
			LOG_ERR("llext_bringup('%s') failed (%d)", entry->name, ret);
			llext_unload(&inst->ext);
			k_free(inst->elf_buf);
			inst->elf_buf = NULL;
			return ret;
		}
	} else {
		LOG_DBG("'%s' image already resident; sharing it", entry->name);
	}

	sym = llext_find_sym(&inst->ext->exp_tab, ZD_ZAPP_MANIFEST_SYM);
	if (sym == NULL) {
		LOG_ERR("'%s' exports no %s", entry->name, ZD_ZAPP_MANIFEST_SYM);
		unwind_image(inst);
		return -ENOEXEC;
	}

	manifest = sym;
	ret = validate_manifest(manifest, entry->path);
	if (ret != 0) {
		unwind_image(inst);
		return ret;
	}

	/* Checked here rather than before the load, because the flag lives in
	 * the manifest and the manifest lives in the image. unwind_image() just
	 * drops the refcount we took, which is the right thing: the resident
	 * copy belongs to the instance already using it.
	 */
	{
		struct zd_zapp_instance *running = singleton_already_running(manifest);

		if (running != NULL) {
			unwind_image(inst);
			relaunch(running, arg);
			return 0;
		}
	}

	inst->manifest = manifest;
	inst->live = true;
	live_instances++;

	set_launch_arg(inst, arg);

	if (manifest->name != NULL) {
		strncpy(inst->name, manifest->name, sizeof(inst->name) - 1);
		inst->name[sizeof(inst->name) - 1] = '\0';
	}

	LOG_INF("launched '%s' instance %u (ABI %u.%u)", inst->name, inst->id,
		manifest->abi_major, manifest->abi_minor);

	/* Everything the zapp does from here can call back into the host API, so
	 * the guard has to be held across init exactly as across an event.
	 */
	inst->initialising = true;
	loader_wm->in_zapp_callback++;
	ret = manifest->init(&inst->ctx, zd_host_api_for(inst));
	loader_wm->in_zapp_callback--;
	inst->initialising = false;

	if (ret != 0) {
		LOG_ERR("'%s' init failed (%d); unloading", inst->name, ret);
		zd_zapp_request_unload(inst);
		return ret;
	}

	/* Creating a window focuses it, which would otherwise deliver
	 * ZD_EV_WINDOW_FOCUS while init() is still running -- before the zapp has
	 * had a chance to record the handle it was just given, so it cannot yet
	 * recognise its own window. Events are suppressed during init and the
	 * focus state is re-asserted here, once the zapp is fully constructed.
	 */
	if (loader_wm->focused != NULL && loader_wm->focused->owner == inst) {
		struct zd_event ev = {
			.type = ZD_EV_WINDOW_FOCUS,
			.win = (zd_window_t)loader_wm->focused->handle,
		};

		zd_zapp_dispatch(inst, &ev);
	}

	return 0;
}

/* --- launching from a zapp ------------------------------------------------------- */

/*
 * One queued launch, drained from the desktop loop.
 *
 * One rather than a queue because the thing being asked for is a user action --
 * somebody double-clicked a document -- and two of those cannot arrive in one
 * frame. A second within the same frame gets -EBUSY, which is a truthful answer
 * to "you already asked".
 */
static struct {
	bool valid;
	char name[ZD_ZAPP_NAME_MAX];
	char arg[ZD_PATH_MAX];
} pending_launch;

/** Find a discovered zapp by name. @return true with @p out filled. */
static bool find_zapp(const char *name, struct zd_zapp_entry *out)
{
	static struct zd_zapp_entry found[ZD_MAX_DISCOVERED];
	int count = zd_zapps_discover(loader_session, found, ARRAY_SIZE(found));

	for (int i = 0; i < count; i++) {
		if (strcmp(found[i].name, name) == 0) {
			*out = found[i];
			return true;
		}
	}

	return false;
}

int zd_zapp_launch_request(const char *name, const char *arg)
{
	struct zd_zapp_entry entry;

	if (name == NULL || name[0] == '\0') {
		return -EINVAL;
	}

	if (arg != NULL && strlen(arg) >= sizeof(pending_launch.arg)) {
		return -EINVAL;
	}

	if (pending_launch.valid) {
		return -EBUSY;
	}

	/* Everything answerable now is answered now, so a caller's mistake comes
	 * back from the call that made it rather than arriving as a log line a
	 * frame later. The picker does the same with its directory, and for the
	 * same reason. What is left asynchronous is only what cannot be known
	 * without doing the work: a corrupt ELF, a refused ABI, llext running
	 * out of heap. Those are logged, exactly as they already are when the
	 * Start menu hits them -- main.c has always discarded that return value.
	 */
	if (!find_zapp(name, &entry)) {
		LOG_WRN("no zapp named '%s' was discovered", name);
		return -ENOENT;
	}

	if (alloc_slot() == NULL) {
		return -ENOMEM;
	}

	(void)strncpy(pending_launch.name, entry.name, sizeof(pending_launch.name) - 1);
	pending_launch.name[sizeof(pending_launch.name) - 1] = '\0';

	if (arg != NULL) {
		strcpy(pending_launch.arg, arg);
	} else {
		pending_launch.arg[0] = '\0';
	}

	pending_launch.valid = true;
	return 0;
}

void zd_zapp_launch_reap(void)
{
	struct zd_zapp_entry entry;

	if (!pending_launch.valid || loader_wm == NULL ||
	    loader_wm->in_zapp_callback > 0) {
		return;
	}

	pending_launch.valid = false;

	/* Re-resolved rather than remembered: discovery is cheap, and the path
	 * recorded a frame ago could have been unlinked by whatever asked for
	 * the launch -- a file browser is exactly the zapp that might.
	 */
	if (!find_zapp(pending_launch.name, &entry)) {
		LOG_WRN("'%s' is gone since it was asked for", pending_launch.name);
		return;
	}

	(void)zd_zapp_launch(&entry, pending_launch.arg);
}

void zd_zapp_dispatch(struct zd_zapp_instance *inst, const struct zd_event *ev)
{
	if (!inst->live || inst->initialising || inst->pending_unload ||
	    inst->manifest->event == NULL) {
		return;
	}

	inst->wm->in_zapp_callback++;
	inst->manifest->event(&inst->ctx, ev);
	inst->wm->in_zapp_callback--;
}

void zd_zapp_request_unload(struct zd_zapp_instance *inst)
{
	if (!inst->live || inst->pending_unload) {
		return;
	}

	inst->pending_unload = true;
	LOG_DBG("instance %u '%s' queued for unload", inst->id, inst->name);
}

void zd_zapp_window_gone(struct zd_zapp_instance *inst)
{
	if (inst == NULL || !inst->live) {
		return;
	}

	if (inst->window_count > 0) {
		inst->window_count--;
	}

	/* A zapp with no windows left has nothing to interact with. Closing the
	 * last window is how you quit.
	 */
	if (inst->window_count == 0) {
		zd_zapp_request_unload(inst);
	}
}

void zd_zapp_on_client_destroyed(struct zd_client *client)
{
	struct zd_zapp_instance *inst = client->owner;

	if (client->handle != 0) {
		zd_handle_free(client->handle);
		client->handle = 0;
	}

	zd_zapp_window_gone(inst);
}

void zd_zapp_on_client_focus(struct zd_client *client, bool focused)
{
	struct zd_zapp_instance *inst = client->owner;
	struct zd_event ev = {
		.type = focused ? ZD_EV_WINDOW_FOCUS : ZD_EV_WINDOW_BLUR,
		.win = (zd_window_t)client->handle,
	};

	if (inst == NULL || client->handle == 0) {
		return; /* desktop-internal window */
	}

	zd_zapp_dispatch(inst, &ev);
}

void zd_zapp_on_client_click(struct zd_client *client, int16_t x, int16_t y)
{
	struct zd_zapp_instance *inst = client->owner;
	struct zd_event ev = {
		.type = ZD_EV_CLICK,
		.win = (zd_window_t)client->handle,
		.click = { .x = x, .y = y },
	};

	if (inst == NULL || client->handle == 0) {
		return; /* desktop-internal window */
	}

	zd_zapp_dispatch(inst, &ev);
}

void zd_zapp_on_client_resized(struct zd_client *client, int16_t w, int16_t h)
{
	struct zd_zapp_instance *inst = client->owner;
	struct zd_event ev = {
		.type = ZD_EV_RESIZED,
		.win = (zd_window_t)client->handle,
		.resize = { .w = w, .h = h },
	};

	if (inst == NULL || client->handle == 0) {
		return; /* desktop-internal window */
	}

	zd_zapp_dispatch(inst, &ev);
}

void zd_zapp_on_client_key(struct zd_client *client, uint32_t code, uint32_t unicode,
			   uint16_t mods)
{
	struct zd_zapp_instance *inst = client->owner;
	struct zd_event ev = {
		.type = ZD_EV_KEY,
		.win = (zd_window_t)client->handle,
		.key = { .code = code, .unicode = unicode, .mods = mods },
	};

	if (inst == NULL || client->handle == 0) {
		return; /* desktop-internal window */
	}

	zd_zapp_dispatch(inst, &ev);
}

void zd_zapp_on_client_minimized(struct zd_client *client, bool minimized)
{
	struct zd_zapp_instance *inst = client->owner;
	struct zd_event ev = {
		.type = minimized ? ZD_EV_MINIMIZED : ZD_EV_RESTORED,
		.win = (zd_window_t)client->handle,
	};

	if (inst == NULL || client->handle == 0) {
		return; /* desktop-internal window */
	}

	zd_zapp_dispatch(inst, &ev);
}

bool zd_zapp_on_client_close_stalled(struct zd_client *client)
{
	/* The WM has no idea what a dialog is and the dialog code has no idea
	 * what a client is; the loader is the only place that knows both, which
	 * is why the hook lands here rather than either of them.
	 */
	return client->owner != NULL && zd_dialog_open_for(client->owner);
}

bool zd_zapp_on_client_close_request(struct zd_client *client)
{
	struct zd_zapp_instance *inst = client->owner;
	struct zd_event ev = {
		.type = ZD_EV_WINDOW_CLOSE_REQUEST,
		.win = (zd_window_t)client->handle,
	};

	/* Say no to the WM whenever nothing could act on the event, so the close
	 * happens now instead of stalling for the whole grace period. That is a
	 * desktop-internal window, a zapp with no event callback, or an instance
	 * that is already on its way out -- zd_zapp_dispatch() would drop the
	 * event in the last case, and the window would hang until the deadline.
	 */
	if (inst == NULL || client->handle == 0 || inst->manifest->event == NULL ||
	    !inst->live || inst->initialising || inst->pending_unload) {
		return false;
	}

	zd_zapp_dispatch(inst, &ev);
	return true;
}

static void finish_unload(struct zd_zapp_instance *inst)
{
	LOG_DBG("unloading instance %u '%s'", inst->id, inst->name);

	if (inst->manifest != NULL && inst->manifest->fini != NULL) {
		inst->wm->in_zapp_callback++;
		inst->manifest->fini(&inst->ctx);
		inst->wm->in_zapp_callback--;
	}

	/* A dialog this instance asked for has nobody left to answer it, and a
	 * modal shade with no owner leaves the whole desktop unclickable. Take
	 * it down before anything else goes.
	 */
	zd_dialog_owner_gone(inst);

	/* Files first, and only after fini() -- which is a zapp's last chance to
	 * flush. zd_handle_free_all() below only invalidates handles; on its own
	 * it would leave the filesystem holding the file objects behind them open
	 * for the rest of the boot.
	 */
	zd_fs_close_all(inst);

	/* Any handle the zapp still holds dies with it; the generation bump means
	 * a stale copy can never resolve, even into a later instance's slot.
	 */
	zd_handle_free_all(inst);

	/* Only the last instance sharing this image tears it down. */
	unwind_image(inst);

	inst->manifest = NULL;
	inst->live = false;
	inst->pending_unload = false;
	live_instances--;

	LOG_INF("unloaded instance %u; %u zapp(s) live, %u handle(s) live, %u file(s) open",
		inst->id, live_instances, zd_handle_live_count(), zd_fs_open_count());
}

void zd_zapp_reap(void)
{
	/* Same rule as the window reap, for the same reason -- only more so.
	 * llext_unload() frees the very text a zapp frame would return into.
	 */
	if (loader_wm == NULL || loader_wm->in_zapp_callback > 0) {
		return;
	}

	for (size_t i = 0; i < MAX_INSTANCES; i++) {
		struct zd_zapp_instance *inst = &slots[i].inst;

		if (!inst->live || !inst->pending_unload) {
			continue;
		}

		/* Windows first: the zapp's LVGL objects must be gone before its
		 * code is. The WM reap runs before this one in the loop.
		 */
		if (inst->window_count > 0) {
			continue;
		}

		finish_unload(inst);
	}
}

uint32_t zd_zapp_instance_count(void)
{
	return live_instances;
}
