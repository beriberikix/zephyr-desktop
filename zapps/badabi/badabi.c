/*
 * badabi — an app the desktop must refuse to run.
 *
 * Deliberately declares an ABI major one ahead of the desktop's. Shipped and
 * installed alongside the working apps so the rejection path is exercised on
 * every boot by anyone who clicks it, rather than being a test that rots in a
 * directory nobody runs.
 *
 * If this ever launches successfully, the version gate is broken.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/llext/symbol.h>

#include <zd/app_abi.h>

static int badabi_init(zd_app_ctx_t ctx, const struct zd_host_api *api)
{
	/* Never reached: the loader rejects the manifest before calling init. */
	(void)ctx;
	(void)api;
	return -1;
}

struct zd_app_manifest zd_app_manifest = {
	.magic = ZD_APP_MAGIC,
	.abi_major = ZD_ABI_MAJOR + 1, /* the whole point */
	.abi_minor = 0,
	.flags = 0,
	.name = "Bad ABI",
	.icon = NULL,
	.init = badabi_init,
	.event = NULL,
	.fini = NULL,
};
LL_EXTENSION_SYMBOL(zd_app_manifest);
