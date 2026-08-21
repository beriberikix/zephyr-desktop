/*
 * clippy — the paperclip.
 *
 * Drawn as text rather than as a bitmap, for two reasons. The ABI has no image
 * call, and adding one to draw a paperclip would be the wrong reason to grow
 * it. And a text widget is already the thing the desktop can animate cheaply:
 * a frame change is one text_set_text(), which is a single LVGL label update,
 * where a grid would be a full redraw of a surface built for Minesweeper
 * boards.
 *
 * Frames are picked to read at 8x16 in the desktop's mono font. The eyes carry
 * the expression; the body never moves, so the eye is the only thing the reader
 * tracks between frames and the animation stays legible at this size.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "clippy.h"

/*
 * Idle: a slow blink. Two frames open, one narrowed, one shut -- weighted so
 * the eyes are open most of the time, because a paperclip that blinks evenly
 * looks like it is malfunctioning rather than waiting.
 */
static const char *const idle_frames[] = {
	"  _\n"
	" //\\\n"
	" |  |\n"
	" \\  /   o o\n"
	"  \\/\n",

	"  _\n"
	" //\\\n"
	" |  |\n"
	" \\  /   o o\n"
	"  \\/\n",

	"  _\n"
	" //\\\n"
	" |  |\n"
	" \\  /   - -\n"
	"  \\/\n",

	"  _\n"
	" //\\\n"
	" |  |\n"
	" \\  /   o o\n"
	"  \\/\n",
};

/*
 * Thinking: the paperclip leans, and the eyes travel. This is the frame set
 * that actually matters -- the wait is tens of seconds, and a still image for
 * that long reads as a hung program.
 */
static const char *const busy_frames[] = {
	"   _\n"
	"  //\\\n"
	"  |  |\n"
	"  \\  /  o o\n"
	"   \\/\n",

	"  _\n"
	" //\\\n"
	" |  |\n"
	" \\  /   o o\n"
	"  \\/\n",

	" _\n"
	"//\\\n"
	"|  |\n"
	"\\  /    o o\n"
	" \\/\n",

	"  _\n"
	" //\\\n"
	" |  |\n"
	" \\  /  o o\n"
	"  \\/\n",
};

/* Answered: eyes wide. One frame -- the reader's attention should be on the
 * answer, not on the paperclip.
 */
static const char *const done_frame =
	"  _\n"
	" //\\\n"
	" |  |\n"
	" \\  /   O O\n"
	"  \\/\n";

/* Failed: flat eyes. Distinct at a glance from both waiting and answered,
 * which is the whole job -- a user who glances over should not have to read
 * the text to know it went wrong.
 */
static const char *const sad_frame =
	"  _\n"
	" //\\\n"
	" |  |\n"
	" \\  /   x x\n"
	"  \\/\n";

const char *clippy_face(enum clippy_mood mood, uint32_t tick)
{
	switch (mood) {
	case CLIPPY_BUSY:
		return busy_frames[tick % (sizeof(busy_frames) / sizeof(busy_frames[0]))];
	case CLIPPY_DONE:
		return done_frame;
	case CLIPPY_SAD:
		return sad_frame;
	case CLIPPY_IDLE:
	default:
		return idle_frames[tick % (sizeof(idle_frames) / sizeof(idle_frames[0]))];
	}
}
