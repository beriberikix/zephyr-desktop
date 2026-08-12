/*
 * zephyr-desktop — generation-counted handle registry.
 *
 * A zapp never holds a struct zd_client *. It holds a handle that encodes a
 * slot index and a generation counter; every host-API call resolves it and
 * checks that the slot is live, that the generation still matches, and that the
 * object belongs to the calling instance.
 *
 * Without an MMU this is the *only* part of the isolation story that genuinely
 * works. It does not stop a malicious extension -- nothing here does -- but it
 * turns the overwhelmingly common failure, a zapp using a handle after closing
 * it, from a use-after-free into -EINVAL. It also stops zapp A touching zapp B's
 * windows by guessing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_WM_HANDLE_H_
#define ZD_WM_HANDLE_H_

#include <stdint.h>

#include <zd/zapp_abi.h>

struct zd_client;
struct zd_zapp_instance;

/*
 * The registry is not LVGL-specific, despite living in wm/. Files and
 * directories go through the same slots, the same generation counter and the
 * same owner check as windows -- which is the first evidence that the scheme
 * generalises past the WM at all.
 */
enum zd_handle_kind {
	ZD_HANDLE_WINDOW,
	ZD_HANDLE_LABEL,
	ZD_HANDLE_FILE,
	ZD_HANDLE_DIR,
	ZD_HANDLE_TEXT,
	ZD_HANDLE_MENU,
	ZD_HANDLE_LIST,
};

/** Register an object and return its handle, or 0 if the table is full. */
uintptr_t zd_handle_alloc(enum zd_handle_kind kind, void *object,
			  struct zd_zapp_instance *owner);

/**
 * @brief Resolve a handle.
 *
 * @param owner instance making the call, or NULL to skip the ownership check
 *              (desktop-internal callers only).
 * @return the object, or NULL if the handle is stale, forged, of the wrong
 *         kind, or owned by someone else.
 */
void *zd_handle_deref(uintptr_t handle, enum zd_handle_kind kind,
		      struct zd_zapp_instance *owner);

/** Invalidate a handle. Its generation advances, so the old value never works. */
void zd_handle_free(uintptr_t handle);

/** Invalidate every handle belonging to @p owner. Used at instance teardown. */
void zd_handle_free_all(struct zd_zapp_instance *owner);

/** Live handle count, for leak assertions. */
uint32_t zd_handle_live_count(void);

#endif /* ZD_WM_HANDLE_H_ */
