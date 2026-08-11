/*
 * hello — the smallest possible zephyr-desktop app.
 *
 * Built as a real .llext, discovered on the filesystem, loaded at runtime, and
 * linked against exactly one desktop symbol. Everything it can do, it does
 * through the host API table handed to init().
 *
 * Note what is NOT here: no LVGL, no Zephyr headers, no knowledge of where its
 * window comes from or what draws it. That is the ABI doing its job.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/llext/symbol.h>

#include <zd/app_abi.h>

static const struct zd_host_api *host;
static zd_window_t window;
static zd_label_t line;

static int hello_init(zd_app_ctx_t ctx, const struct zd_host_api *api)
{
	struct zd_window_desc desc = {
		.title = "Hello",
		.geom = { 0, 0, 0, 0 }, /* let the desktop place and cascade us */
	};

	host = api;

	if (api->abi_major != ZD_ABI_MAJOR) {
		return -1; /* the loader checks this too; belt and braces */
	}

	/* An app does not actually need to import anything -- init() is handed
	 * the table. This call exists to exercise the other direction anyway:
	 * it forces the loader to resolve a symbol out of the desktop's export
	 * table at link time, so a broken EXPORT_GROUP_SYMBOL fails loudly here
	 * rather than silently the first time some later app depends on it.
	 */
	if (zd_get_host_api() == NULL) {
		return -1;
	}

	window = api->window_create(ctx, &desc);
	if (window == NULL) {
		return -1;
	}

	line = api->label_create(ctx, window, "hello world", 8, 8);
	if (line == NULL) {
		return -1;
	}

	api->log(ctx, 0, "hello world");
	return 0;
}

static void hello_event(zd_app_ctx_t ctx, const struct zd_event *ev)
{
	/* Proves the event path is real rather than aspirational: the title
	 * tracks focus, which the desktop only ever tells us about by calling
	 * this function.
	 */
	switch (ev->type) {
	case ZD_EV_WINDOW_FOCUS:
		host->window_set_title(ctx, ev->win, "Hello (active)");
		break;
	case ZD_EV_WINDOW_BLUR:
		host->window_set_title(ctx, ev->win, "Hello");
		break;
	default:
		break;
	}
}

static void hello_fini(zd_app_ctx_t ctx)
{
	host->log(ctx, 0, "goodbye");
	window = NULL;
	line = NULL;
}

struct zd_app_manifest zd_app_manifest = {
	.magic = ZD_APP_MAGIC,
	.abi_major = ZD_ABI_MAJOR,
	.abi_minor = ZD_ABI_MINOR,
	.flags = 0, /* untrusted: no unsafe_lvgl_content for us */
	.name = "Hello",
	.icon = NULL,
	.init = hello_init,
	.event = hello_event,
	.fini = hello_fini,
};
LL_EXTENSION_SYMBOL(zd_app_manifest);
