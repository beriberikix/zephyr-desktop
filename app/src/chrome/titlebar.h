/*
 * zephyr-desktop — titlebar chrome.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_CHROME_TITLEBAR_H_
#define ZD_CHROME_TITLEBAR_H_

#include <lvgl.h>

/** Build the titlebar styles. Called by zd_theme_init(). */
void zd_titlebar_styles_init(void);

/** Apply the focused or unfocused titlebar fill. */
void zd_titlebar_set_active(lv_obj_t *titlebar, bool active);

/** Draw the close button's glyph in DRAW_POST, on top of its bevel. */
void zd_close_glyph_attach(lv_obj_t *obj);

/** The same, for the minimise button. */
void zd_minimize_glyph_attach(lv_obj_t *obj);

extern lv_style_t zd_style_title_active;
extern lv_style_t zd_style_title_inactive;

#endif /* ZD_CHROME_TITLEBAR_H_ */
