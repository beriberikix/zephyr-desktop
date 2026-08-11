/*
 * zephyr-desktop — the app ABI.
 *
 * This header is the contract between the desktop and every extension. It
 * deliberately includes no Zephyr and no LVGL headers, only <stdint.h> and
 * <stddef.h>. That is what keeps LVGL's ABI from silently becoming ours, and
 * what would let an app be linked statically behind the same contract if a
 * non-llext-capable target ever mattered again.
 *
 * Versioning rule: abi_major must match exactly; app.abi_minor <= host.abi_minor
 * is accepted. The host vtable only ever grows by appending, and struct_size
 * lets an older app bind safely against a newer host. Anything else is a major
 * bump.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_APP_ABI_H_
#define ZD_APP_ABI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZD_ABI_MAJOR 0
#define ZD_ABI_MINOR 1

/** Longest absolute path the desktop will hand back or accept. */
#define ZD_PATH_MAX 96

/** Longest app display name. */
#define ZD_APP_NAME_MAX 24

/* Opaque handles. An app never sees an lv_obj_t. */
typedef struct zd_app_ctx *zd_app_ctx_t;
typedef struct zd_window *zd_window_t;
typedef struct zd_label *zd_label_t;

struct zd_rect {
	int16_t x;
	int16_t y;
	int16_t w;
	int16_t h;
};

/**
 * Well-known directories, resolved per session.
 *
 * Apps never build absolute paths themselves: the filesystem root differs
 * between targets (FATFS wants a "/RAM:" volume prefix under QEMU, an SD volume
 * on hardware) and the home directory belongs to the session, not the app.
 */
enum zd_dir {
	ZD_DIR_HOME,        /**< read-write */
	ZD_DIR_SYSTEM_APPS, /**< read-only */
	ZD_DIR_USER_APPS,   /**< read-write */
	ZD_DIR_TMP,         /**< read-write */
};

#ifdef __cplusplus
}
#endif

#endif /* ZD_APP_ABI_H_ */
