/*
 * mines — shared state, command ids and the board's private encoding.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZAPPS_MINES_MINES_H_
#define ZAPPS_MINES_MINES_H_

#include <stdbool.h>

#include <lib/zapplib.h>
#include <zd/zapp_abi.h>

/*
 * How many games can run at once, from this zapp's side.
 *
 * Its own number, deliberately not CONFIG_ZD_MAX_GRIDS -- the same rule
 * notepad.h states at length. Two, because each game holds two grids and the
 * desktop's table is small; a third would be refused the window, which is the
 * correct failure and not a nice one to look at.
 */
#define MS_MAX_INSTANCES 2

/*
 * Largest board this zapp will carry state for.
 *
 * Expert is 30x16, so this covers every standard level and there is no point
 * being larger: what actually decides the board is grid_fit(), and no panel
 * this project targets can show 480 cells at touch sizing. Ours, not the
 * desktop's -- grid_get_capacity() is asked for separately and the smaller of
 * the two wins.
 */
#define MS_MAX_CELLS 480

/*
 * One cell, packed into a byte.
 *
 * The low nibble is how many mines touch it, 0..8, which is why the flags start
 * at 0x10. There is no "wrong flag" bit: that is FLAG set with MINE clear, and
 * deriving it is one fewer thing that can disagree with itself.
 */
#define MS_MINE  0x80u
#define MS_FLAG  0x40u
#define MS_OPEN  0x20u
#define MS_BOOM  0x10u /**< the one that was stepped on, drawn differently */
#define MS_COUNT 0x0Fu

enum ms_phase {
	MS_READY,   /**< laid out, but no mines yet -- see ms_first_click() */
	MS_PLAYING,
	MS_WON,
	MS_LOST,
};

struct ms_level {
	uint8_t cols;
	uint8_t rows;
	uint16_t mines;
	const char *name;
};

/* Menu command ids. Ours; the desktop only hands them back. */
#define MS_GAME_NEW   1
#define MS_GAME_EXIT  2
#define MS_LEVEL_BASE 10 /**< +0 Beginner, +1 Intermediate, +2 Expert */

/** Timer ids. Also ours. */
#define MS_TIMER_TICK 1

struct ms_state {
	bool used;
	zd_window_t win;

	/**
	 * The board, and the panel above it.
	 *
	 * The panel is a grid too, one cell tall: three digits, a face, three
	 * digits, and blank filler between. That is not cleverness for its own
	 * sake -- it is what Minesweeper's panel has always been, and doing it
	 * with two labels and a button would have needed a button, which this
	 * ABI does not have and does not need to grow.
	 */
	zd_grid_t board;
	zd_grid_t panel;
	zd_menu_t game_menu;

	uint8_t cols;
	uint8_t rows;
	uint8_t panel_cols;
	uint8_t level;

	uint16_t mines;   /**< laid, or about to be */
	uint16_t flags;   /**< how many the player has planted */
	uint16_t opened;  /**< uncovered squares, for the win test */
	enum ms_phase phase;

	int64_t started_ms;
	uint16_t shown_secs;

	uint32_t rng;

	int16_t content_w;
	int16_t content_h;

	/**
	 * The biggest content area this window can have, learned once at
	 * init() by asking for an absurd one and reading back what arrived.
	 * There is no screen-size call in the ABI, and this is why there does
	 * not need to be: what a zapp may have is what the desktop will give
	 * it, which is a different and more useful number.
	 */
	int16_t max_w;
	int16_t max_h;
	uint16_t cap; /**< cells: the smaller of the desktop's and ours */

	uint8_t cell[MS_MAX_CELLS];
	/**
	 * The flood fill's work list.
	 *
	 * An explicit queue rather than recursion, and not for tidiness: a zapp
	 * runs as a callback on the desktop thread, so its stack is the desktop
	 * loop's, and an empty region on a 29x10 board is nearly three hundred
	 * frames deep. Every square is enqueued at most once -- MS_OPEN is set
	 * as it goes in -- so this bound is exact rather than hopeful.
	 */
	uint16_t queue[MS_MAX_CELLS];
};

extern const struct zd_host_api *ms_host;
extern const struct ms_level ms_levels[3];

/* --- board.c --------------------------------------------------------------------- */

/**
 * @brief Choose a board for @p level that fits in @p max_cols by @p max_rows.
 *
 * Pure, and answering through pointers rather than writing the state, so the
 * caller can try to reshape the grid FIRST and only commit if that worked. A
 * state that says 16x16 over a grid still holding 9x9 would index off the end
 * of one of them.
 *
 * Clamped down, never up: a Beginner board on a small panel is a smaller
 * Beginner board, not a wider one. The mine count follows the area so the
 * density -- which is what the difficulty actually is -- survives the clamp.
 */
void ms_choose_shape(uint8_t level, uint8_t max_cols, uint8_t max_rows, uint16_t cap,
		     uint8_t *cols, uint8_t *rows, uint16_t *mines);

/** Empty the board and go back to MS_READY. Does not lay mines. */
void ms_reset(struct ms_state *st);

/** Uncover (col,row). @return true if anything about the game changed. */
bool ms_reveal(zd_zapp_ctx_t ctx, struct ms_state *st, uint8_t col, uint8_t row);

/** Plant or lift a flag. @return true if anything changed. */
bool ms_toggle_flag(struct ms_state *st, uint8_t col, uint8_t row);

/* --- render.c -------------------------------------------------------------------- */

/** Push the whole board model into the grid. */
void ms_draw_board(zd_zapp_ctx_t ctx, struct ms_state *st);

/** Push one square. Called from the flood fill, which touches many. */
void ms_draw_cell(zd_zapp_ctx_t ctx, struct ms_state *st, uint8_t col, uint8_t row);

/** Redraw the counters and the face. */
void ms_draw_panel(zd_zapp_ctx_t ctx, struct ms_state *st);

#endif /* ZAPPS_MINES_MINES_H_ */
