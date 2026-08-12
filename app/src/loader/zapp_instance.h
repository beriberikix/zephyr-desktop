/*
 * zephyr-desktop — a running zapp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_LOADER_ZAPP_INSTANCE_H_
#define ZD_LOADER_ZAPP_INSTANCE_H_

#include <zephyr/llext/llext.h>

#include <zd/zapp_abi.h>

#include "zapp_loader.h"
#include "../host/session.h"
#include "../wm/wm.h"

/** Per-instance context. The zapp holds this as an opaque zd_zapp_ctx_t. */
struct zd_zapp_ctx {
	struct zd_zapp_instance *inst;
};

struct zd_zapp_instance {
	struct llext *ext;
	struct llext_loader *loader;
	const struct zd_zapp_manifest *manifest;

	struct zd_session *session;
	struct zd_wm *wm;
	struct zd_zapp_ctx ctx;

	uint32_t id;
	uint32_t window_count;
	void *user_data; /**< the app's per-instance slot; see zd_host_api */
	char name[ZD_ZAPP_NAME_MAX];

	void *elf_buf;   /**< heap copy of the ELF, when loading via buffer */
	size_t elf_size;

	/**
	 * What this instance was launched with, or "".
	 *
	 * Kept for the instance's life rather than only until init() returns: a
	 * zapp may reasonably want it again -- to re-title a window, to retry a
	 * failed open -- and 192 bytes is cheaper than making it copy the string
	 * out defensively. Replaced when a singleton is re-launched.
	 */
	char launch_arg[ZD_PATH_MAX];

	bool live;
	bool initialising; /**< inside init(); no events may be delivered yet */
	bool pending_unload;
	bool owns_image; /**< true if this instance was the one that loaded the ELF */
};

/** Wire the loader to its WM and session. Call once at boot. */
void zd_zapp_loader_init(struct zd_wm *wm, struct zd_session *session);

/**
 * @brief Load, link, bring up and initialise an extension.
 *
 * discover -> load -> bringup -> bind manifest -> validate ABI -> init.
 * Any failure unwinds everything already done and returns non-zero.
 */
int zd_zapp_launch(const struct zd_zapp_entry *entry, const char *arg);

/**
 * @brief Ask for a zapp to be launched by name, from the desktop loop.
 *
 * What a zapp calls, and the difference from zd_zapp_launch() is when the work
 * happens rather than what it does. Everything checkable is checked now and
 * comes back as a return code; the load itself waits for zd_zapp_launch_reap().
 *
 * Deferred because the caller is usually another zapp, and doing it inline
 * would (a) re-enter that zapp with a ZD_EV_WINDOW_BLUR while its own event()
 * frame is on the stack, when the new window takes focus, and (b) read fifteen
 * kilobytes off the filesystem from inside a zapp callback -- which on the
 * CoreS3 means taking the display's pin back mid-dispatch.
 *
 * @param name a DISCOVERED ZAPP NAME, not a path. Resolved through the same
 *             scan the Start menu uses, so this cannot be talked into loading
 *             an arbitrary file.
 * @param arg  what to hand the new instance, or NULL.
 *
 * @return 0 once queued, -ENOENT, -ENOMEM, -EBUSY or -EINVAL.
 */
int zd_zapp_launch_request(const char *name, const char *arg);

/** Carry out a queued launch. From the desktop loop only. */
void zd_zapp_launch_reap(void);

/** Copy this instance's launch argument out. @return its length, or -ENOSPC. */
int zd_zapp_get_launch_arg(struct zd_zapp_instance *inst, char *buf, uint32_t len);

/**
 * @brief Mark an instance for teardown.
 *
 * Like window close, never inline: the caller is usually inside a zapp callback
 * on a stack frame that lives in the extension's own text. The unload happens
 * from zd_zapp_reap().
 */
void zd_zapp_request_unload(struct zd_zapp_instance *inst);

/** Deliver an event to a zapp, with the in_zapp_callback guard held. */
void zd_zapp_dispatch(struct zd_zapp_instance *inst, const struct zd_event *ev);

/** Called by the WM when a window owned by an instance is destroyed. */
void zd_zapp_window_gone(struct zd_zapp_instance *inst);

/* Hooks installed on the WM at boot, so the WM never has to know zapps exist. */
void zd_zapp_on_client_destroyed(struct zd_client *client);
void zd_zapp_on_client_focus(struct zd_client *client, bool focused);
void zd_zapp_on_client_click(struct zd_client *client, int16_t x, int16_t y);
void zd_zapp_on_client_resized(struct zd_client *client, int16_t w, int16_t h);
void zd_zapp_on_client_minimized(struct zd_client *client, bool minimized);
void zd_zapp_on_client_key(struct zd_client *client, uint32_t code, uint32_t unicode,
			   uint16_t mods);

/**
 * @brief Ask a window's zapp to close itself.
 *
 * @return true if the request was delivered and the WM should wait for the
 *         grace period; false if there was nobody to ask, in which case the
 *         window should be closed immediately rather than stalling.
 */
bool zd_zapp_on_client_close_request(struct zd_client *client);

/**
 * @brief Is this window's zapp visibly asking the user about the close?
 *
 * Installed as wm->on_client_close_stalled. True buys another grace period,
 * because a zapp showing "save changes?" is doing what the close request asked
 * of it, not ignoring it.
 */
bool zd_zapp_on_client_close_stalled(struct zd_client *client);

/** Finish teardowns. Runs from the desktop loop, never from dispatch. */
void zd_zapp_reap(void);

/** Live instance count, for leak assertions. */
uint32_t zd_zapp_instance_count(void);

#endif /* ZD_LOADER_ZAPP_INSTANCE_H_ */
