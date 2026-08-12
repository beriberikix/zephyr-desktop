/*
 * zephyr-desktop — the cell grid, as zapps see it.
 *
 * chrome/cellgrid.c is the widget; this is the layer that gives it handles,
 * ownership and a way to reach a zapp. The same split list_api.h describes over
 * rowlist and text_api.h over lv_textarea, kept for the same reason -- although
 * this one has no second customer yet. It is here anyway, because the
 * alternative is a widget that only a zapp can have, and the desktop's own
 * chrome has wanted a keypad since the on-screen keyboard was written.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_GRID_API_H_
#define ZD_HOST_GRID_API_H_

#include <stdint.h>

#include <zd/zapp_abi.h>

struct zd_client;
struct zd_zapp_instance;

uintptr_t zd_grid_create(struct zd_zapp_instance *owner, struct zd_client *client,
			 int16_t x, int16_t y, uint8_t cols, uint8_t rows);
void zd_grid_destroy(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_grid_set_pos(struct zd_zapp_instance *owner, uintptr_t handle, int16_t x,
		    int16_t y);
int zd_grid_resize(struct zd_zapp_instance *owner, uintptr_t handle, uint8_t cols,
		   uint8_t rows);
int zd_grid_set_cell(struct zd_zapp_instance *owner, uintptr_t handle, uint8_t col,
		     uint8_t row, const char *text, uint32_t style, uint32_t rgb);
int zd_grid_clear(struct zd_zapp_instance *owner, uintptr_t handle);

/** Live grid widgets, for leak assertions. */
uint32_t zd_grid_live_count(void);

#endif /* ZD_HOST_GRID_API_H_ */
