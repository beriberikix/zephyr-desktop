/*
 * zephyr-desktop — the assistant balloon, as zapps see it.
 *
 * chrome/balloon.c is the widget; this is the handle, ownership and validation
 * layer over it. The same split grid_api.h describes over cellgrid and
 * list_api.h over rowlist.
 *
 * Thinner than either, because a balloon sends no events. Nothing in it is
 * clickable, so there is no callback going the other way and no question about
 * what a zapp may do from inside one.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_BALLOON_API_H_
#define ZD_HOST_BALLOON_API_H_

#include <stdint.h>

#include <zd/zapp_abi.h>

struct zd_client;
struct zd_zapp_instance;

uintptr_t zd_balloon_api_create(struct zd_zapp_instance *owner, struct zd_client *client,
				int16_t x, int16_t y, int16_t w, int16_t h,
				uint32_t icon);
void zd_balloon_api_destroy(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_balloon_api_set_text(struct zd_zapp_instance *owner, uintptr_t handle,
			    const char *text);
int zd_balloon_api_set_geometry(struct zd_zapp_instance *owner, uintptr_t handle,
				int16_t x, int16_t y, int16_t w, int16_t h);
int zd_balloon_api_set_icon(struct zd_zapp_instance *owner, uintptr_t handle,
			    uint32_t icon, uint32_t state);

/** Live balloon widgets, for leak assertions. */
uint32_t zd_balloon_api_live_count(void);

#endif /* ZD_HOST_BALLOON_API_H_ */
