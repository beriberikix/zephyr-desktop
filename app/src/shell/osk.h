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

/**
 * @brief How much of the screen the keyboard is taking, in pixels.
 *
 * 0 when it is down. Anything laying itself out on the overlay has to ask:
 * the keyboard is a fixture at the bottom of the screen rather than a window,
 * and on a 320x240 panel it is over half of it. A dialog sized as though it
 * were not there puts its own text field behind the keys.
 */
int32_t zd_osk_height(void);

/** What it would be if it were up, whether it is or not. For fit checks. */
int32_t zd_osk_max_height(void);

/**
 * @brief Move the keyboard above whatever was just raised on the overlay.
 *
 * The overlay is one layer and everything modal is on it, so "on top" is child
 * order and the last thing to move wins. A dialog's shade eats every press
 * outside its panel, so a keyboard left underneath it can be seen and not
 * used — which is exactly what happened when a dialog opened while the
 * keyboard was already up, because set_visible() had nothing to do and so did
 * not re-raise it.
 */
void zd_osk_raise(void);

/**
 * @brief Let the keyboard use the taskbar's strip as well.
 *
 * On while a system-modal dialog is up, and only then. The taskbar is
 * unreachable behind the shade anyway, so its 28 px are free — and on a 240 px
 * panel those 28 px are the difference between a Save As box that fits above
 * the keys and one that does not.
 */
void zd_osk_cover_taskbar(bool cover);

#else

#define zd_osk_init(layers)      ((void)0)
#define zd_osk_set_visible(v)    ((void)0)
#define zd_osk_toggle()          ((void)0)
#define zd_osk_visible()         false
#define zd_osk_wanted(w)         ((void)0)
#define zd_osk_height()          0
#define zd_osk_max_height()      0
#define zd_osk_raise()           ((void)0)
#define zd_osk_cover_taskbar(c)  ((void)0)

#endif /* CONFIG_ZD_OSK */

#endif /* ZD_SHELL_OSK_H_ */
