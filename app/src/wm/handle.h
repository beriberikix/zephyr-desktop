/*
 * zephyr-desktop — generation-counted handle registry.
 *
 * An app never holds a struct zd_client *. It holds a handle that encodes a
 * slot index and a generation counter; every host-API call resolves it and
 * checks that the slot is live, that the generation still matches, and that the
 * object belongs to the calling instance.
 *
 * Without an MMU this is the *only* part of the isolation story that genuinely
 * works. It does not stop a malicious extension -- nothing here does -- but it
 * turns the overwhelmingly common failure, an app using a handle after closing
 * it, from a use-after-free into -EINVAL. It also stops app A touching app B's
 * windows by guessing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_WM_HANDLE_H_
#define ZD_WM_HANDLE_H_

#include <stdint.h>

#include <zd/app_abi.h>

struct zd_client;
struct zd_app_instance;

enum zd_handle_kind {
	ZD_HANDLE_WINDOW,
	ZD_HANDLE_LABEL,
};

/** Register an object and return its handle, or 0 if the table is full. */
uintptr_t zd_handle_alloc(enum zd_handle_kind kind, void *object,
			  struct zd_app_instance *owner);

/**
 * @brief Resolve a handle.
 *
 * @param owner instance making the call, or NULL to skip the ownership check
 *              (desktop-internal callers only).
 * @return the object, or NULL if the handle is stale, forged, of the wrong
 *         kind, or owned by someone else.
 */
void *zd_handle_deref(uintptr_t handle, enum zd_handle_kind kind,
		      struct zd_app_instance *owner);

/** Invalidate a handle. Its generation advances, so the old value never works. */
void zd_handle_free(uintptr_t handle);

/** Invalidate every handle belonging to @p owner. Used at instance teardown. */
void zd_handle_free_all(struct zd_app_instance *owner);

/** Live handle count, for leak assertions. */
uint32_t zd_handle_live_count(void);

#endif /* ZD_WM_HANDLE_H_ */
