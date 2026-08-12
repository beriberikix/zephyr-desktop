/*
 * zephyr-desktop — a real keyboard, translated.
 *
 * Zephyr's input subsystem reports Linux key codes: scancodes with a US layout
 * baked into their numbering and no notion of what character they produce. This
 * turns them into the desktop's own ZD_KEY_* codes and, for anything printable,
 * a codepoint.
 *
 * It is a US layout and says so. A real one would come from a table the session
 * chooses; this is the smallest thing that lets Notepad be typed into, and the
 * shape it would grow into is a second pair of `runs` arrays.
 *
 * Note what this does NOT do: register an LVGL keypad indev. LVGL's keypad
 * driver maps a fixed handful of codes to LV_KEY_* for navigating widgets with
 * a d-pad, which is a different problem. Routing belongs to the desktop -- see
 * wm/keys.c -- so the input subsystem hands its events here and here hands them
 * to the one funnel.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zd/zapp_abi.h>

#include "keys.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/*
 * The printable runs of the Linux keycode space, in layout order.
 *
 * Linux numbers the main typing block by physical position, so each row is a
 * contiguous run and the whole US layout is four strings. A per-code switch
 * would be a hundred cases saying the same thing less clearly.
 */
struct run {
	uint16_t first;
	const char *lower;
	const char *upper;
	bool letters; /**< caps lock applies to this run */
};

static const struct run runs[] = {
	{ 2,  "1234567890-=", "!@#$%^&*()_+", false },
	{ 16, "qwertyuiop[]", "QWERTYUIOP{}", true },
	{ 30, "asdfghjkl;'`", "ASDFGHJKL:\"~", true },
	{ 43, "\\",           "|",            false },
	{ 44, "zxcvbnm,./",   "ZXCVBNM<>?",   true },
	{ 57, " ",            " ",            false },
};

static bool caps_lock;

/** @return the codepoint for @p code, or 0 if it is not a printable key. */
static uint32_t printable(uint16_t code, bool shift)
{
	for (size_t i = 0; i < ARRAY_SIZE(runs); i++) {
		const struct run *run = &runs[i];
		size_t len = strlen(run->lower);

		if (code < run->first || code >= run->first + len) {
			continue;
		}

		/* Caps lock is shift for letters and nothing at all for the
		 * digit row, which is the behaviour of every keyboard anyone
		 * has used and none of the behaviour a naive XOR would give.
		 */
		if (run->letters && caps_lock) {
			shift = !shift;
		}

		return (uint32_t)(uint8_t)(shift ? run->upper : run->lower)[code - run->first];
	}

	return 0;
}

/** @return the ZD_KEY_* value for a named key, or 0 if @p code is not one. */
static uint32_t named(uint16_t code)
{
	switch (code) {
	case INPUT_KEY_BACKSPACE: return ZD_KEY_BACKSPACE;
	case INPUT_KEY_TAB:       return ZD_KEY_TAB;
	case INPUT_KEY_ENTER:
	case INPUT_KEY_KPENTER:   return ZD_KEY_ENTER;
	case INPUT_KEY_ESC:       return ZD_KEY_ESCAPE;
	case INPUT_KEY_DELETE:    return ZD_KEY_DELETE;
	case INPUT_KEY_LEFT:      return ZD_KEY_LEFT;
	case INPUT_KEY_RIGHT:     return ZD_KEY_RIGHT;
	case INPUT_KEY_UP:        return ZD_KEY_UP;
	case INPUT_KEY_DOWN:      return ZD_KEY_DOWN;
	case INPUT_KEY_HOME:      return ZD_KEY_HOME;
	case INPUT_KEY_END:       return ZD_KEY_END;
	case INPUT_KEY_PAGEUP:    return ZD_KEY_PAGE_UP;
	case INPUT_KEY_PAGEDOWN:  return ZD_KEY_PAGE_DOWN;
	case INPUT_KEY_INSERT:    return ZD_KEY_INSERT;
	default:
		break;
	}

	if (code >= INPUT_KEY_F1 && code <= INPUT_KEY_F10) {
		return ZD_KEY_F(1 + code - INPUT_KEY_F1);
	}
	if (code == INPUT_KEY_F11) {
		return ZD_KEY_F(11);
	}
	if (code == INPUT_KEY_F12) {
		return ZD_KEY_F(12);
	}

	return 0;
}

/** @return the ZD_MOD_* bit @p code is, or 0 if it is not a modifier. */
static uint16_t modifier(uint16_t code)
{
	switch (code) {
	case INPUT_KEY_LEFTSHIFT:
	case INPUT_KEY_RIGHTSHIFT:
		return ZD_MOD_SHIFT;
	case INPUT_KEY_LEFTCTRL:
	case INPUT_KEY_RIGHTCTRL:
		return ZD_MOD_CTRL;
	case INPUT_KEY_LEFTALT:
	case INPUT_KEY_RIGHTALT:
		return ZD_MOD_ALT;
	default:
		return 0;
	}
}

static void keymap_cb(struct input_event *evt, void *user_data)
{
	uint16_t mod;
	uint32_t code;
	uint32_t unicode;

	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY) {
		return;
	}

	/*
	 * Pointer buttons come through INPUT_EV_KEY too -- the virtio tablet
	 * reports INPUT_BTN_TOUCH on every tap, and on the CoreS3 so does the
	 * touch panel. Linux puts every button at 0x100 -- INPUT_BTN_0, the
	 * first of them -- and above, and every key below it, so one comparison
	 * separates the two modalities without this file having to know which
	 * devices exist on this board.
	 */
	if (evt->code >= INPUT_BTN_0) {
		return;
	}

	mod = modifier(evt->code);
	if (mod != 0) {
		zd_keys_modifier(mod, evt->value != 0);
		return;
	}

	/* value: 0 release, 1 press, 2 auto-repeat. Repeats are presses. */
	if (evt->value == 0) {
		return;
	}

	if (evt->code == INPUT_KEY_CAPSLOCK) {
		caps_lock = !caps_lock;
		return;
	}

	code = named(evt->code);
	if (code != 0) {
		zd_keys_press(code, 0);
		return;
	}

	unicode = printable(evt->code, (zd_keys_mods() & ZD_MOD_SHIFT) != 0);
	if (unicode != 0) {
		zd_keys_press(ZD_KEY_CHAR, unicode);
	}
}

/*
 * Every input device, rather than a named one.
 *
 * The keyboard is a different devicetree node on every board that has one, and
 * two of the three targets have none at all. Filtering by code above is both
 * portable and honest about what this cares about: keys, wherever they came
 * from.
 */
INPUT_CALLBACK_DEFINE(NULL, keymap_cb, NULL);
