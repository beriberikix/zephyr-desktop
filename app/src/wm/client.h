/*
 * zephyr-desktop — client subtree construction, internal to the WM.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_WM_CLIENT_H_
#define ZD_WM_CLIENT_H_

#include "wm.h"

/** Build the frame/titlebar/close/content subtree under @p parent. */
void zd_client_build(struct zd_client *client, lv_obj_t *parent);

/** Delete the subtree. Only legal from zd_wm_reap(). */
void zd_client_destroy_widgets(struct zd_client *client);

/** Push client->geom onto LVGL. */
void zd_client_apply_geom(struct zd_client *client);

/**
 * @brief The content area's size, derived from client->geom.
 *
 * Derived, not read back. lv_obj_set_size() only marks an object dirty -- the
 * coordinates are not recomputed until LVGL's next layout pass -- so asking
 * LVGL for the size immediately after a resize returns the size it had before.
 * The model knows, and the model is the one that is allowed to.
 */
void zd_client_content_size(const struct zd_client *client, int16_t *w, int16_t *h);

#endif /* ZD_WM_CLIENT_H_ */
