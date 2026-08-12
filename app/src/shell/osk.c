/*
 * zephyr-desktop — a keyboard for boards that have none.
 *
 * The CoreS3 is a 320x240 touch panel with no keys at all, and it is the target
 * that has actually run this project on silicon. Without this, Notepad is a
 * QEMU-only application and the hardware story quietly becomes "a desktop you
 * can look at". So this is not a convenience widget; it is what makes a
 * touch-only board a first-class target.
 *
 * It calls zd_keys_press() and zd_keys_modifier() and does nothing else.
 * Everything downstream -- the routing in wm/keys.c, the text widget, the zapp
 * -- cannot tell its keys from the virtio keyboard's, which is the entire point
 * of there being one funnel in input/keys.h.
 *
 * Two deliberate departures from the rest of the chrome:
 *
 *   - It is an lv_buttonmatrix rather than forty bevelled lv_obj_ts. A matrix
 *     lays out one contiguous grid with exact adjacent rectangles, which is the
 *     right shape for the hit-slop lesson this project has now learned twice
 *     (see ITEM_H in launcher.c and ZD_BTN_SZ in wm/wm.h): keys are made
 *     bigger, and no key claims area outside itself. Forty separate objects
 *     would each need their own bevel callback and could drift out of
 *     alignment.
 *   - Its buttons are therefore flat with a single-tone border, not Win95
 *     bevels, because a bevel is a per-object draw callback and a matrix has
 *     one object. An on-screen keyboard is an anachronism in a Win95 shell
 *     anyway; better an honest flat one than a fake bevel.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zd/zapp_abi.h>

#include "dialog.h"
#include "osk.h"
#include "taskbar.h"
#include "../chrome/theme.h"
#include "../input/keys.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/*
 * Key height. The touch allowance is real pixels, as everywhere else in this
 * project: 20 px on a pointer-driven target, 32 on the CoreS3, so four rows are
 * 80 px of a 272 px screen or 128 px of a 240 px one. The second number is why
 * this is a toggle rather than always on.
 */
#define KEY_H (20 + CONFIG_ZD_TOUCH_SLOP_PX)
#define ROWS  4
#define OSK_H (ROWS * KEY_H)

/* Labels that are not the character they send. Kept short so ten fit across a
 * 320 px panel; "Ent" and "Bsp" are what a real 1990s soft keyboard did too.
 */
#define K_SHIFT "Sh"
#define K_CTRL  "Ctl"
#define K_BKSP  "Bsp"
#define K_ENTER "Ent"
#define K_ESC   "Esc"
#define K_LEFT  "<"
#define K_RIGHT ">"
#define K_PAGE1 "abc"
#define K_PAGE2 "12#"
#define K_SPACE " "
/*
 * Put the keyboard away.
 *
 * The taskbar's toggle is the other way to do it, and on this panel that is
 * both a long reach from where your thumb already is and, while a modal dialog
 * is up, underneath something. A keyboard you cannot dismiss from the keyboard
 * is a keyboard that can trap you.
 *
 * Bottom-left, as far from Enter as the row allows: a mis-tap here costs the
 * keyboard, and the key it must not be adjacent to is the one people aim at
 * without looking.
 */
#define K_HIDE  "Hide"

static const char *const map_lower[] = {
	"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
	"a", "s", "d", "f", "g", "h", "j", "k", "l", K_BKSP, "\n",
	K_SHIFT, "z", "x", "c", "v", "b", "n", "m", ",", ".", "\n",
	K_HIDE, K_PAGE2, K_CTRL, K_ESC, K_SPACE, K_LEFT, K_RIGHT, K_ENTER, "",
};

static const char *const map_upper[] = {
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
	"A", "S", "D", "F", "G", "H", "J", "K", "L", K_BKSP, "\n",
	K_SHIFT, "Z", "X", "C", "V", "B", "N", "M", ";", ":", "\n",
	K_HIDE, K_PAGE2, K_CTRL, K_ESC, K_SPACE, K_LEFT, K_RIGHT, K_ENTER, "",
};

static const char *const map_sym[] = {
	"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
	"-", "=", "[", "]", "\\", ";", "'", "/", "?", K_BKSP, "\n",
	K_SHIFT, "!", "@", "#", "$", "%", "&", "*", "(", ")", "\n",
	K_HIDE, K_PAGE1, K_CTRL, K_ESC, K_SPACE, K_LEFT, K_RIGHT, K_ENTER, "",
};

/*
 * Relative widths. Rows are independent, so only the last one needs saying:
 * ten single-width keys above, then a bottom row where the space bar is worth
 * six of them. LV_BUTTONMATRIX_CTRL_CHECKABLE is deliberately absent -- Shift
 * and Ctrl are one-shot and their state is shown by relabelling the whole
 * keyboard, which is more legible on a small panel than a single lit key.
 */
static const lv_buttonmatrix_ctrl_t map_ctrl[] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	3, 2, 2, 2, 5, 2, 2, 3,
};

static struct {
	lv_obj_t *kb;
	bool open;
	bool shift;
	bool ctrl;
	bool symbols;
	bool over_taskbar;
} osk;

/*
 * Sit on the bottom edge, above the taskbar unless something has said the
 * taskbar is unreachable anyway.
 *
 * Re-applied rather than set once, because the strip it may use changes while a
 * dialog is up. Cheap: two calls, and only when the answer changes.
 */
static void place(void)
{
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
	int32_t screen_h = lv_display_get_vertical_resolution(NULL);
	int32_t below = osk.over_taskbar ? 0 : ZD_TASKBAR_H;

	lv_obj_set_size(osk.kb, screen_w, OSK_H);
	lv_obj_set_pos(osk.kb, 0, screen_h - below - OSK_H);
}

static void apply_map(void)
{
	const char *const *map = osk.symbols ? map_sym
			       : osk.shift   ? map_upper
					     : map_lower;

	lv_buttonmatrix_set_map(osk.kb, map);
	lv_buttonmatrix_set_ctrl_map(osk.kb, map_ctrl);
}

/*
 * Shift and Ctrl are one-shot: held for exactly the next key, then dropped.
 *
 * A latch would be more faithful to a real keyboard and much worse to use with
 * one thumb -- every Ctrl+S would need three deliberate taps and leave the
 * desktop in a modified state if the third missed. One-shot means the mistake
 * costs one wrong character instead of every subsequent one.
 */
static void clear_oneshots(void)
{
	if (!osk.shift && !osk.ctrl) {
		return;
	}

	osk.shift = false;
	osk.ctrl = false;
	zd_keys_modifier(ZD_MOD_SHIFT, false);
	zd_keys_modifier(ZD_MOD_CTRL, false);
	apply_map();
}

static bool named_key(const char *text, uint32_t *code)
{
	if (strcmp(text, K_BKSP) == 0) {
		*code = ZD_KEY_BACKSPACE;
	} else if (strcmp(text, K_ENTER) == 0) {
		*code = ZD_KEY_ENTER;
	} else if (strcmp(text, K_ESC) == 0) {
		*code = ZD_KEY_ESCAPE;
	} else if (strcmp(text, K_LEFT) == 0) {
		*code = ZD_KEY_LEFT;
	} else if (strcmp(text, K_RIGHT) == 0) {
		*code = ZD_KEY_RIGHT;
	} else {
		return false;
	}

	return true;
}

static void key_pressed(lv_event_t *e)
{
	uint32_t id = lv_buttonmatrix_get_selected_button(osk.kb);
	const char *text = lv_buttonmatrix_get_button_text(osk.kb, id);
	uint32_t code;

	if (text == NULL || text[0] == '\0') {
		return;
	}

	ARG_UNUSED(e);

	if (strcmp(text, K_SHIFT) == 0) {
		osk.shift = !osk.shift;
		zd_keys_modifier(ZD_MOD_SHIFT, osk.shift);
		apply_map();
		return;
	}

	if (strcmp(text, K_CTRL) == 0) {
		osk.ctrl = !osk.ctrl;
		zd_keys_modifier(ZD_MOD_CTRL, osk.ctrl);
		return;
	}

	if (strcmp(text, K_HIDE) == 0) {
		zd_osk_set_visible(false);
		return;
	}

	if (strcmp(text, K_PAGE1) == 0 || strcmp(text, K_PAGE2) == 0) {
		osk.symbols = !osk.symbols;
		apply_map();
		return;
	}

	if (named_key(text, &code)) {
		zd_keys_press(code, 0);
	} else {
		/* Every remaining label is the single character it sends. */
		zd_keys_press(ZD_KEY_CHAR, (uint32_t)(uint8_t)text[0]);
	}

	clear_oneshots();
}

void zd_osk_init(struct zd_layers *layers)
{
	osk.kb = lv_buttonmatrix_create(layers->overlay);
	lv_obj_remove_style_all(osk.kb);
	place();
	lv_obj_add_flag(osk.kb, LV_OBJ_FLAG_HIDDEN);

	/* Never focusable, and never a click target for the WM. The window
	 * being typed into has to stay focused, and stay looking focused, while
	 * its own keyboard is being used.
	 */
	lv_obj_remove_flag(osk.kb, LV_OBJ_FLAG_CLICK_FOCUSABLE);
	lv_buttonmatrix_set_one_checked(osk.kb, false);

	lv_obj_set_style_bg_color(osk.kb, lv_color_hex(ZD_C_FACE), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(osk.kb, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_pad_all(osk.kb, 1, LV_PART_MAIN);

	lv_obj_set_style_bg_color(osk.kb, lv_color_hex(ZD_C_FACE), LV_PART_ITEMS);
	lv_obj_set_style_bg_opa(osk.kb, LV_OPA_COVER, LV_PART_ITEMS);
	lv_obj_set_style_border_color(osk.kb, lv_color_hex(ZD_C_SHADOW), LV_PART_ITEMS);
	lv_obj_set_style_border_width(osk.kb, 1, LV_PART_ITEMS);
	lv_obj_set_style_radius(osk.kb, 0, LV_PART_ITEMS);
	lv_obj_set_style_text_color(osk.kb, lv_color_hex(ZD_C_TEXT), LV_PART_ITEMS);
	lv_obj_set_style_text_font(osk.kb, &lv_font_montserrat_12, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(osk.kb, lv_color_hex(ZD_C_SHADOW),
				  LV_PART_ITEMS | LV_STATE_PRESSED);

	apply_map();
	lv_obj_add_event_cb(osk.kb, key_pressed, LV_EVENT_VALUE_CHANGED, NULL);
}

void zd_osk_set_visible(bool visible)
{
	if (osk.open == visible) {
		return;
	}

	osk.open = visible;

	if (visible) {
		lv_obj_remove_flag(osk.kb, LV_OBJ_FLAG_HIDDEN);
		lv_obj_move_to_index(osk.kb, -1);
	} else {
		lv_obj_add_flag(osk.kb, LV_OBJ_FLAG_HIDDEN);
		/* Modifiers do not survive the keyboard going away: a latched
		 * Ctl nobody can see would silently turn the next real
		 * keystroke into an accelerator.
		 */
		clear_oneshots();
	}

	/* A dialog sized itself around the keyboard, so it has to be told the
	 * answer changed -- on a small panel this is what lets Hide give a Save
	 * As box its file list back.
	 */
	zd_dialog_relayout();

	zd_taskbar_set_osk_active(visible);
}

void zd_osk_toggle(void)
{
	zd_osk_set_visible(!osk.open);
}

bool zd_osk_visible(void)
{
	return osk.open;
}

int32_t zd_osk_height(void)
{
	return osk.open ? OSK_H : 0;
}

int32_t zd_osk_max_height(void)
{
	return OSK_H;
}

void zd_osk_raise(void)
{
	if (osk.kb != NULL) {
		lv_obj_move_to_index(osk.kb, -1);
	}
}

void zd_osk_cover_taskbar(bool cover)
{
	if (osk.over_taskbar == cover || osk.kb == NULL) {
		return;
	}

	osk.over_taskbar = cover;
	place();
}

void zd_osk_wanted(bool wanted)
{
	if (!IS_ENABLED(CONFIG_ZD_OSK_AUTO)) {
		return;
	}

	/* Only ever raises. Lowering it because a text widget lost the caret
	 * would fight the user every time they reached for the taskbar.
	 */
	if (wanted) {
		zd_osk_set_visible(true);
	}
}
