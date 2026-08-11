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

#endif /* ZD_WM_CLIENT_H_ */
