/*
 * notes — a second app, to prove the desktop runs more than one.
 *
 * Where hello is the minimum, this one exercises the parts of the ABI hello
 * does not: path resolution, and more than one window per instance. It keeps
 * opening windows until the desktop refuses, so the per-app quota is visible
 * rather than merely asserted.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/llext/symbol.h>

#include <zd/app_abi.h>

#define MAX_TRIES 8

static const struct zd_host_api *host;

static int notes_init(zd_app_ctx_t ctx, const struct zd_host_api *api)
{
	char home[ZD_PATH_MAX];
	zd_window_t first = NULL;
	int opened = 0;

	host = api;

	for (int i = 0; i < MAX_TRIES; i++) {
		struct zd_window_desc desc = {
			.title = "Notes",
			.geom = { 0, 0, 0, 0 },
		};
		zd_window_t win = api->window_create(ctx, &desc);

		if (win == NULL) {
			break; /* quota reached -- expected, not an error */
		}

		if (first == NULL) {
			first = win;
		}
		opened++;
	}

	if (first == NULL) {
		return -1;
	}

	/* The app has no idea what the filesystem root is called; that is the
	 * session's business. It asks.
	 */
	if (api->path_resolve(ctx, ZD_DIR_HOME, home, sizeof(home)) == 0) {
		api->label_create(ctx, first, home, 6, 8);
		api->log(ctx, 0, home);
	}

	api->label_create(ctx, first, opened == 1 ? "1 window" : "windows open", 6, 26);
	return 0;
}

static void notes_fini(zd_app_ctx_t ctx)
{
	host->log(ctx, 0, "notes closed");
}

struct zd_app_manifest zd_app_manifest = {
	.magic = ZD_APP_MAGIC,
	.abi_major = ZD_ABI_MAJOR,
	.abi_minor = ZD_ABI_MINOR,
	.flags = 0,
	.name = "Notes",
	.icon = NULL,
	.init = notes_init,
	.event = NULL, /* an app need not care about events */
	.fini = notes_fini,
};
LL_EXTENSION_SYMBOL(zd_app_manifest);
