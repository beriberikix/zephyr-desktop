/*
 * zephyr-desktop — the on-screen keyboard.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_SHELL_OSK_H_
#define ZD_SHELL_OSK_H_

#include <stdbool.h>

#include "desktop.h"

#ifdef CONFIG_ZD_OSK

/** Build the keyboard, hidden. Call once, after the layers exist. */
void zd_osk_init(struct zd_layers *layers);

void zd_osk_set_visible(bool visible);
void zd_osk_toggle(void);
bool zd_osk_visible(void);

/**
 * @brief Raise the keyboard because something now wants typing.
 *
 * A no-op where the board is presumed to have a real keyboard -- see
 * CONFIG_ZD_OSK_AUTO. Called when a text widget takes the caret, so that on a
 * touch panel the thing you just tapped into can actually be typed into,
 * without a second deliberate trip to the taskbar.
 */
void zd_osk_wanted(bool wanted);

#else

#define zd_osk_init(layers)      ((void)0)
#define zd_osk_set_visible(v)    ((void)0)
#define zd_osk_toggle()          ((void)0)
#define zd_osk_visible()         false
#define zd_osk_wanted(w)         ((void)0)

#endif /* CONFIG_ZD_OSK */

#endif /* ZD_SHELL_OSK_H_ */
