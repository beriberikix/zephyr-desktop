/*
 * zephyr-desktop — the taskbar's window list.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_SHELL_TASKLIST_H_
#define ZD_SHELL_TASKLIST_H_

#include <lvgl.h>

struct zd_wm;

/** Create the (empty) button row on the panel layer. */
void zd_tasklist_init(lv_obj_t *panel, struct zd_wm *wm);

/**
 * @brief Note that the window list needs rebuilding. Cheap and reentrant.
 *
 * Only sets a flag. THE REBUILD MUST NOT HAPPEN HERE.
 *
 * This is the WM's on_client_list_changed hook, and the most direct way to
 * reach it is by clicking a taskbar button -- which changes focus, which fires
 * the hook, from inside the LV_EVENT_CLICKED dispatch of the very button a
 * rebuild would delete. Same trap as a zapp closing its own window from its own
 * callback, same answer: queue it, and let the top of the desktop loop do the
 * deleting. See CLAUDE.md.
 */
void zd_tasklist_invalidate(struct zd_wm *wm);

/** Rebuild if invalidated. Call from the desktop loop, never from dispatch. */
void zd_tasklist_reap(void);

#endif /* ZD_SHELL_TASKLIST_H_ */
