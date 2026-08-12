/*
 * zephyr-desktop — the one place a key enters the system.
 *
 * There is deliberately a single funnel, matching what focus.c's header comment
 * has claimed since milestone D: "a future keyboard or encoder modality routes
 * through the same funnel rather than inheriting per-widget LVGL behaviour."
 *
 * Two sources call in here and neither knows the other exists: a real keyboard,
 * via the Zephyr input subsystem in keymap.c, and the on-screen keyboard in
 * shell/osk.c. Everything downstream -- the routing policy, the text widget,
 * the zapp -- sees one stream and cannot tell them apart. That is what makes a
 * touch-only board a first-class target rather than a degraded one.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_INPUT_KEYS_H_
#define ZD_INPUT_KEYS_H_

#include <stdbool.h>
#include <stdint.h>

struct zd_wm;

/** Wire the key path to the window manager. Call once at boot. */
void zd_keys_init(struct zd_wm *wm);

/**
 * @brief A modifier went down or came up.
 *
 * @param mod one of ZD_MOD_SHIFT / ZD_MOD_CTRL / ZD_MOD_ALT.
 *
 * Held state lives here rather than in each source, because the two sources
 * express it differently -- a real keyboard sends press and release, the
 * on-screen keyboard latches a button -- and everything downstream wants the
 * same answer from both.
 */
void zd_keys_modifier(uint16_t mod, bool held);

/**
 * @brief One key press, already mapped.
 *
 * @param code    a ZD_KEY_* value.
 * @param unicode the codepoint, when @p code is ZD_KEY_CHAR; 0 otherwise.
 *
 * Presses only. A source that sees releases drops them; nothing downstream
 * wants them, and they would double the traffic.
 *
 * QUEUES rather than delivers. Safe to call from any thread and from an ISR;
 * the routing, and everything it touches in LVGL, happens later on the desktop
 * thread. See the header comment in keys.c for what happened the first time it
 * did not.
 */
void zd_keys_press(uint32_t code, uint32_t unicode);

/**
 * @brief Deliver every queued key press.
 *
 * Called from the desktop loop with the LVGL lock held, next to the reaps and
 * for the same reason: this is where it is legal to touch LVGL and to re-enter
 * a zapp.
 */
void zd_keys_pump(void);

/** The modifiers currently held, as a mask of ZD_MOD_*. */
uint16_t zd_keys_mods(void);

/** Drop every latched modifier. Called when a source loses its grip on state. */
void zd_keys_clear_mods(void);

#endif /* ZD_INPUT_KEYS_H_ */
