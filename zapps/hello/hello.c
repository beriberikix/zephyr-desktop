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

/*
 * `host` is genuinely per-app: every instance is handed the same table, so a
 * file-scope variable is correct here.
 *
 * Per-INSTANCE state is a different matter. llext loads an image once and
 * refcounts it, so launching this app twice shares this file's .data and .bss
 * between both instances -- a second `static zd_window_t window` would be
 * silently overwritten by whichever instance started last. Anything per
 * instance goes through set_user_data()/get_user_data().
 */
static const struct zd_host_api *host;

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

	zd_window_t window = api->window_create(ctx, &desc);

	if (window == NULL) {
		return -1;
	}

	if (api->label_create(ctx, window, "hello world, Zephyr!", 8, 8) == NULL) {
		return -1;
	}

	/* The handle is pointer-sized, so it rides in the slot directly and this
	 * app needs no allocation at all.
	 */
	api->set_user_data(ctx, (void *)window);

	api->log(ctx, 0, "hello world, Zephyr!");
	return 0;
}

static void hello_event(zd_app_ctx_t ctx, const struct zd_event *ev)
{
	/* Proves the event path is real rather than aspirational: the title
	 * tracks focus, which the desktop only ever tells us about by calling
	 * this function.
	 */
	zd_window_t mine = (zd_window_t)host->get_user_data(ctx);

	/* Cross-check the event against our own recorded handle. With two
	 * instances sharing this code, getting this wrong is how one instance
	 * ends up retitling the other's window.
	 */
	if (ev->win != mine) {
		return;
	}

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
	host->set_user_data(ctx, NULL);
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
