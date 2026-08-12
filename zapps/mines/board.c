/*
 * mines — the game itself, with no idea that a screen exists.
 *
 * Nothing in this file calls anything that draws. It reads and writes
 * st->cell[] and it starts and stops the clock, and that is deliberate: the
 * rules of Minesweeper are thirty years old and settled, and they are the part
 * worth being able to read without a widget in the way.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mines.h"

const struct ms_level ms_levels[3] = {
	{ .cols = 9, .rows = 9, .mines = 10, .name = "Beginner" },
	{ .cols = 16, .rows = 16, .mines = 40, .name = "Intermediate" },
	{ .cols = 30, .rows = 16, .mines = 99, .name = "Expert" },
};

static uint16_t at_of(const struct ms_state *st, uint8_t col, uint8_t row)
{
	return (uint16_t)row * st->cols + col;
}

static uint16_t total_of(const struct ms_state *st)
{
	return (uint16_t)st->cols * st->rows;
}

/* --- choosing a board -------------------------------------------------------------- */

void ms_choose_shape(uint8_t level, uint8_t max_cols, uint8_t max_rows, uint16_t cap,
		     uint8_t *cols, uint8_t *rows, uint16_t *mines)
{
	const struct ms_level *l = &ms_levels[level < 3 ? level : 0];
	uint32_t want = (uint32_t)l->cols * l->rows;
	uint32_t got;
	uint8_t c = l->cols < max_cols ? l->cols : max_cols;
	uint8_t r = l->rows < max_rows ? l->rows : max_rows;

	/* Shave rows before columns. A board wider than it is tall is what
	 * every one of these levels already is, and a landscape panel is what
	 * every one of these targets already has.
	 */
	while (c > 0 && r > 1 && (uint32_t)c * r > cap) {
		r--;
	}
	while (c > 1 && (uint32_t)c * r > cap) {
		c--;
	}

	if (c == 0 || r == 0 || (uint32_t)c * r > cap) {
		*cols = 0;
		*rows = 0;
		*mines = 0;
		return;
	}

	got = (uint32_t)c * r;

	/* Keep the density rather than the count: ten mines in eighty-one
	 * squares is what "Beginner" means, and ten in forty would be a
	 * different game wearing the same label.
	 */
	*mines = (uint16_t)((uint32_t)l->mines * got / want);
	if (*mines < 1) {
		*mines = 1;
	}
	if (*mines > got - 1) {
		*mines = (uint16_t)(got - 1);
	}

	*cols = c;
	*rows = r;
}

void ms_reset(struct ms_state *st)
{
	/* The whole array, not just the live part: a level change can shrink the
	 * board, and leaving the old game's mines past the new end is the kind
	 * of thing that stays invisible until the board grows again.
	 */
	z_zero(st->cell, sizeof(st->cell));

	st->phase = MS_READY;
	st->flags = 0;
	st->opened = 0;
	st->shown_secs = 0;
	st->started_ms = 0;
}

/* --- laying the mines --------------------------------------------------------------- */

/**
 * The indices of the squares touching (col,row), up to eight of them.
 *
 * The bounds check is the whole content of this function, and it is worth
 * having in exactly one place: getting it wrong wraps the board around its own
 * edge, which produces mine counts that are right in the middle and wrong down
 * the sides -- a bug that is very easy to look at without seeing.
 *
 * @return how many were written to @p out.
 */
static uint8_t neighbours(const struct ms_state *st, uint8_t col, uint8_t row,
			  uint16_t *out)
{
	uint8_t n = 0;

	for (int16_t dr = -1; dr <= 1; dr++) {
		int16_t r = (int16_t)row + dr;

		if (r < 0 || r >= (int16_t)st->rows) {
			continue;
		}

		for (int16_t dc = -1; dc <= 1; dc++) {
			int16_t c = (int16_t)col + dc;

			if ((dc == 0 && dr == 0) || c < 0 || c >= (int16_t)st->cols) {
				continue;
			}

			out[n++] = (uint16_t)(r * (int16_t)st->cols + c);
		}
	}

	return n;
}

static void count_neighbours(struct ms_state *st)
{
	for (uint8_t row = 0; row < st->rows; row++) {
		for (uint8_t col = 0; col < st->cols; col++) {
			uint16_t at = at_of(st, col, row);
			uint16_t nb[8];
			uint8_t count = 0;
			uint8_t n;

			if ((st->cell[at] & MS_MINE) != 0) {
				continue;
			}

			n = neighbours(st, col, row, nb);
			while (n-- > 0) {
				if ((st->cell[nb[n]] & MS_MINE) != 0) {
					count++;
				}
			}

			st->cell[at] = (uint8_t)((st->cell[at] & ~MS_COUNT) | count);
		}
	}
}

/**
 * Lay the mines, avoiding @p safe.
 *
 * AFTER the first click, not before it, which is the one rule of Minesweeper
 * that is not in the rules: opening a mine with your opening move is not a
 * game, it is a coin toss with a lid on. Only the clicked square is spared --
 * what Windows did -- rather than its whole neighbourhood, which later
 * implementations added and which quietly guarantees an opening cascade.
 */
static void lay_mines(struct ms_state *st, uint16_t safe)
{
	uint16_t total = total_of(st);
	uint16_t placed = 0;

	while (placed < st->mines) {
		uint16_t at = (uint16_t)z_rand_below(&st->rng, total);

		if (at == safe || (st->cell[at] & MS_MINE) != 0) {
			continue;
		}

		st->cell[at] |= MS_MINE;
		placed++;
	}

	count_neighbours(st);
}

/* --- uncovering ---------------------------------------------------------------------- */

/**
 * Open @p start and, if it touches nothing, everything it opens onto.
 *
 * Breadth-first over an explicit queue. MS_OPEN is set as a square goes IN
 * rather than as it comes out, which is what makes "at most once" true and
 * therefore what makes st->queue's bound exact -- the version that marks on the
 * way out enqueues the same square from four directions and overruns.
 */
static void flood(struct ms_state *st, uint16_t start)
{
	uint16_t head = 0;
	uint16_t tail = 0;

	st->cell[start] |= MS_OPEN;
	st->opened++;
	st->queue[tail++] = start;

	while (head < tail) {
		uint16_t at = st->queue[head++];
		uint16_t nb[8];
		uint8_t n;

		if ((st->cell[at] & MS_COUNT) != 0) {
			continue; /* a numbered square stops the spread */
		}

		n = neighbours(st, (uint8_t)(at % st->cols), (uint8_t)(at / st->cols), nb);

		while (n-- > 0) {
			/* A flagged neighbour is left alone even though it
			 * cannot be a mine here. The player said it was one;
			 * silently overruling them loses the only record of
			 * what they believed.
			 */
			if ((st->cell[nb[n]] & (MS_OPEN | MS_FLAG)) != 0) {
				continue;
			}

			st->cell[nb[n]] |= MS_OPEN;
			st->opened++;
			st->queue[tail++] = nb[n];
		}
	}
}

static void finish(zd_zapp_ctx_t ctx, struct ms_state *st, enum ms_phase how)
{
	st->phase = how;
	ms_host->timer_stop(ctx, MS_TIMER_TICK);

	if (how == MS_WON) {
		/* Flag whatever is left, which is what the original does and
		 * what makes the counter read 000 at the end.
		 */
		for (uint16_t i = 0; i < total_of(st); i++) {
			if ((st->cell[i] & MS_MINE) != 0) {
				st->cell[i] |= MS_FLAG;
			}
		}
		st->flags = st->mines;
	}

	ms_host->log(ctx, 0, how == MS_WON ? "mines: cleared" : "mines: boom");
}

bool ms_reveal(zd_zapp_ctx_t ctx, struct ms_state *st, uint8_t col, uint8_t row)
{
	uint16_t at;

	if (st->phase == MS_WON || st->phase == MS_LOST) {
		return false;
	}

	if (col >= st->cols || row >= st->rows) {
		return false;
	}

	at = at_of(st, col, row);

	if ((st->cell[at] & (MS_OPEN | MS_FLAG)) != 0) {
		return false;
	}

	if (st->phase == MS_READY) {
		lay_mines(st, at);
		st->phase = MS_PLAYING;
		st->started_ms = ms_host->uptime_ms();
		st->shown_secs = 0;
		/* The clock only exists from here, which is also the only
		 * reason this zapp needs ABI 0.7's timers at all.
		 */
		ms_host->timer_start(ctx, 1000, MS_TIMER_TICK);
	}

	if ((st->cell[at] & MS_MINE) != 0) {
		st->cell[at] |= MS_OPEN | MS_BOOM;
		finish(ctx, st, MS_LOST);
		return true;
	}

	flood(st, at);

	if (st->opened + st->mines == total_of(st)) {
		finish(ctx, st, MS_WON);
	}

	return true;
}

bool ms_toggle_flag(struct ms_state *st, uint8_t col, uint8_t row)
{
	uint16_t at;

	if (st->phase == MS_WON || st->phase == MS_LOST) {
		return false;
	}

	if (col >= st->cols || row >= st->rows) {
		return false;
	}

	at = at_of(st, col, row);

	if ((st->cell[at] & MS_OPEN) != 0) {
		return false;
	}

	if ((st->cell[at] & MS_FLAG) != 0) {
		st->cell[at] &= (uint8_t)~MS_FLAG;
		st->flags--;
	} else {
		/* Over-flagging is allowed -- being wrong about where the mines
		 * are is most of the game. The counter simply floors at zero
		 * rather than the original's negative reading, because three
		 * cells have nowhere to put a minus sign.
		 */
		st->cell[at] |= MS_FLAG;
		st->flags++;
	}

	return true;
}
