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
int zd_zapp_launch(const struct zd_zapp_entry *entry);

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

/** Finish teardowns. Runs from the desktop loop, never from dispatch. */
void zd_zapp_reap(void);

/** Live instance count, for leak assertions. */
uint32_t zd_zapp_instance_count(void);

#endif /* ZD_LOADER_ZAPP_INSTANCE_H_ */
