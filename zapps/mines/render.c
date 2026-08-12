/*
 * mines — turning the board model into cells.
 *
 * The only file here that knows there is a screen, and it is one-way: it reads
 * st->cell[] and calls grid_set_cell(). Nothing here decides anything about the
 * game.
 *
 * The markers are ASCII on purpose. LVGL's built-in fonts carry a set of
 * FontAwesome symbols and none of them is a flag or a mine, so the honest
 * choices were "*" and "F" or an embedded font, and a font is a lot of flash to
 * spend on two glyphs. The colours do the work the shapes cannot: a flag is
 * red, a number is the colour Windows gave it, and the mine you stepped on is
 * the only red star on the board.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mines.h"

/*
 * 1 blue, 2 green, 3 red, 4 navy, 5 maroon, 6 teal, 7 black, 8 grey.
 *
 * Straight from Windows 3.1, and worth copying exactly rather than
 * approximating: on a board of identical squares the colour is how you read a
 * number before you have read it, and anyone who has played this knows that
 * two is green.
 */
static const uint32_t number_rgb[9] = {
	0x000000, 0x0000FF, 0x008000, 0xFF0000, 0x000080,
	0x800000, 0x008080, 0x000000, 0x808080,
};

/* No libc, so the digits are literals rather than something sprintf-shaped. */
static const char *const digit[10] = { "0", "1", "2", "3", "4",
				       "5", "6", "7", "8", "9" };

#define MS_RED 0xFF0000u

void ms_draw_cell(zd_zapp_ctx_t ctx, struct ms_state *st, uint8_t col, uint8_t row)
{
	uint8_t v = st->cell[(uint16_t)row * st->cols + col];
	bool over = st->phase == MS_LOST;
	const char *text = "";
	uint32_t style = ZD_CELL_RAISED;
	uint32_t rgb = 0;

	if ((v & MS_OPEN) != 0) {
		style = ZD_CELL_SUNKEN;

		if ((v & MS_MINE) != 0) {
			/* Only reachable on a loss, and only for the one that
			 * was stepped on -- so it is the single red star.
			 */
			text = "*";
			rgb = (v & MS_BOOM) != 0 ? MS_RED : 0;
		} else {
			text = digit[v & MS_COUNT];
			rgb = number_rgb[v & MS_COUNT];

			if ((v & MS_COUNT) == 0) {
				text = ""; /* an empty square shows nothing */
			}
		}
	} else if ((v & MS_FLAG) != 0) {
		if (over && (v & MS_MINE) == 0) {
			/* Where you were wrong, which is the interesting half
			 * of a lost board.
			 */
			style = ZD_CELL_SUNKEN;
			text = "X";
			rgb = MS_RED;
		} else {
			text = "F";
			rgb = MS_RED;
		}
	} else if (over && (v & MS_MINE) != 0) {
		style = ZD_CELL_SUNKEN;
		text = "*";
	}

	ms_host->grid_set_cell(ctx, st->board, col, row, text, style, rgb);
}

void ms_draw_board(zd_zapp_ctx_t ctx, struct ms_state *st)
{
	for (uint8_t row = 0; row < st->rows; row++) {
		for (uint8_t col = 0; col < st->cols; col++) {
			ms_draw_cell(ctx, st, col, row);
		}
	}
}

/** Three cells of a counter, hundreds first, clamped to 999. */
static void draw_counter(zd_zapp_ctx_t ctx, struct ms_state *st, uint8_t first,
			 uint16_t value)
{
	if (value > 999) {
		value = 999;
	}

	for (uint8_t i = 0; i < 3; i++) {
		uint16_t place = i == 0 ? 100 : (i == 1 ? 10 : 1);

		ms_host->grid_set_cell(ctx, st->panel, (uint8_t)(first + i), 0,
				       digit[(value / place) % 10], ZD_CELL_SUNKEN,
				       MS_RED);
	}
}

void ms_draw_panel(zd_zapp_ctx_t ctx, struct ms_state *st)
{
	uint8_t face_at = (uint8_t)((st->panel_cols - 1) / 2);
	const char *face;

	switch (st->phase) {
	case MS_WON:
		face = ":D";
		break;
	case MS_LOST:
		face = ":(";
		break;
	default:
		face = ":)";
		break;
	}

	/* Everything flat and blank first, then the three parts that are not.
	 * Cheaper to reason about than tracking which filler cells the last
	 * layout used, and the panel is thirty cells at the very most.
	 */
	for (uint8_t i = 0; i < st->panel_cols; i++) {
		ms_host->grid_set_cell(ctx, st->panel, i, 0, "", ZD_CELL_FLAT, 0);
	}

	draw_counter(ctx, st, 0, st->mines > st->flags ? st->mines - st->flags : 0);
	draw_counter(ctx, st, (uint8_t)(st->panel_cols - 3), st->shown_secs);

	ms_host->grid_set_cell(ctx, st->panel, face_at, 0, face, ZD_CELL_RAISED, 0);
}
