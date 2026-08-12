/*
 * zephyr-desktop — the taskbar: launcher button and clock.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_SHELL_TASKBAR_H_
#define ZD_SHELL_TASKBAR_H_

#include <stdbool.h>

#include <lvgl.h>

/** Called when the launcher button is released. NULL until milestone E. */
typedef void (*zd_launcher_cb_t)(void *user_data);

/**
 * @brief Populate the panel layer with the launcher button and the clock.
 *
 * @param panel  the `panel` layer from struct zd_layers
 * @param cb     invoked on launcher click, or NULL for an inert button
 * @param cb_arg opaque argument passed back to @p cb
 */
void zd_taskbar_init(lv_obj_t *panel, zd_launcher_cb_t cb, void *cb_arg);

/** Screen-space rectangle of the launcher button, for anchoring its menu. */
void zd_taskbar_launcher_coords(lv_area_t *out);

/**
 * @brief The strip between the launcher button and the clock, in panel-local
 *        coordinates, for the window list to fill.
 *
 * Exported rather than recomputed by the caller: the taskbar owns where its own
 * furniture sits, and a second copy of that arithmetic would go stale the first
 * time the clock changed width.
 */
void zd_taskbar_list_region(lv_area_t *out);

/**
 * @brief Draw the keyboard button pressed or raised.
 *
 * Called by the on-screen keyboard rather than by the button's own handler,
 * because the keyboard can also be raised by a text widget taking the caret and
 * the button must agree with reality either way.
 */
void zd_taskbar_set_osk_active(bool active);

#endif /* ZD_SHELL_TASKBAR_H_ */
