/*
 * zephyr-desktop — synthetic window drag, for measuring redraw cost.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_DRAGBENCH_H_
#define ZD_DRAGBENCH_H_

#include "wm/wm.h"

#ifdef CONFIG_ZD_DRAG_BENCH

/**
 * @brief Drag a window across the screen and report what each frame cost.
 *
 * Runs once, from the same place the boot checks do, with the LVGL lock held.
 * Leaves the desktop as it found it.
 */
void zd_dragbench_run(struct zd_wm *wm);

#else

#define zd_dragbench_run(wm) ((void)0)

#endif /* CONFIG_ZD_DRAG_BENCH */

#endif /* ZD_DRAGBENCH_H_ */
