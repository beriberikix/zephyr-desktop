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

/**
 * @brief Push client->geom's *position* onto LVGL. One call, one object.
 *
 * What a move needs and all it needs. Kept separate from apply_geom() because
 * a drag calls this once per pointer sample: re-laying out the whole subtree
 * there costs eight LVGL calls and three layout invalidations per sample
 * instead of one reposition, which is enough to overrun the touch driver's
 * event queue on a 20 ms poll and make dragging stutter.
 */
void zd_client_apply_pos(struct zd_client *client);

/** Push all of client->geom onto LVGL, re-laying out the subtree. For resize. */
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
