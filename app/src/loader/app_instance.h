/*
 * zephyr-desktop — a running app.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_LOADER_APP_INSTANCE_H_
#define ZD_LOADER_APP_INSTANCE_H_

#include <zephyr/llext/llext.h>

#include <zd/app_abi.h>

#include "app_loader.h"
#include "../host/session.h"
#include "../wm/wm.h"

/** Per-instance context. The app holds this as an opaque zd_app_ctx_t. */
struct zd_app_ctx {
	struct zd_app_instance *inst;
};

struct zd_app_instance {
	struct llext *ext;
	struct llext_loader *loader;
	const struct zd_app_manifest *manifest;

	struct zd_session *session;
	struct zd_wm *wm;
	struct zd_app_ctx ctx;

	uint32_t id;
	uint32_t window_count;
	char name[ZD_APP_NAME_MAX];

	bool live;
	bool pending_unload;
};

/** Wire the loader to its WM and session. Call once at boot. */
void zd_app_loader_init(struct zd_wm *wm, struct zd_session *session);

/**
 * @brief Load, link, bring up and initialise an extension.
 *
 * discover -> load -> bringup -> bind manifest -> validate ABI -> init.
 * Any failure unwinds everything already done and returns non-zero.
 */
int zd_app_launch(const struct zd_app_entry *entry);

/**
 * @brief Mark an instance for teardown.
 *
 * Like window close, never inline: the caller is usually inside an app callback
 * on a stack frame that lives in the extension's own text. The unload happens
 * from zd_app_reap().
 */
void zd_app_request_unload(struct zd_app_instance *inst);

/** Deliver an event to an app, with the in_app_callback guard held. */
void zd_app_dispatch(struct zd_app_instance *inst, const struct zd_event *ev);

/** Called by the WM when a window owned by an instance is destroyed. */
void zd_app_window_gone(struct zd_app_instance *inst);

/* Hooks installed on the WM at boot, so the WM never has to know apps exist. */
void zd_app_on_client_destroyed(struct zd_client *client);
void zd_app_on_client_focus(struct zd_client *client, bool focused);

/** Finish teardowns. Runs from the desktop loop, never from dispatch. */
void zd_app_reap(void);

/** Live instance count, for leak assertions. */
uint32_t zd_app_instance_count(void);

#endif /* ZD_LOADER_APP_INSTANCE_H_ */
