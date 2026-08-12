/*
 * zephyr-desktop — the three-layer screen structure.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_SHELL_DESKTOP_H_
#define ZD_SHELL_DESKTOP_H_

#include <lvgl.h>

/** Height of the taskbar strip along the bottom, in pixels. */
#define ZD_TASKBAR_H 28

/**
 * The four sibling layers on the active screen. Their order is created once
 * and never permuted; only the children of `windows` are ever restacked.
 *
 * Because the order is fixed, the taskbar can never be occluded by a window and
 * the background can never be raised above one -- two whole classes of stacking
 * bug that simply cannot occur.
 */
struct zd_layers {
	lv_obj_t *desktop; /**< patterned background; a click here defocuses */
	lv_obj_t *windows; /**< client frames live here; z-order == child order */
	lv_obj_t *panel;   /**< taskbar */

	/**
	 * Above everything, including the taskbar.
	 *
	 * Home to the things that are not windows and must not be treated as
	 * windows: the Start menu, menu drop-downs, the on-screen keyboard, and
	 * modal dialogs and their click-swallowing shade. Each of those used to
	 * want its own reparenting-onto-the-screen trick; one layer replaces
	 * all of them, and it is what makes a modal dialog modal by
	 * construction rather than by everyone agreeing to behave.
	 *
	 * Not clickable itself, so a press on empty overlay falls through to
	 * whatever is beneath.
	 */
	lv_obj_t *overlay;
};

/** Create the layers on the active screen. Requires zd_theme_init() first. */
void zd_desktop_init(struct zd_layers *layers);

#endif /* ZD_SHELL_DESKTOP_H_ */
