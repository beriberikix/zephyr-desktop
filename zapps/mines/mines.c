/*
 * mines — Minesweeper, and the OS underneath it.
 *
 * The third zapp anybody would recognise, and the one that asked the ABI a
 * question the other two never did: how does a zapp draw something the desktop
 * has no widget for? Notepad wanted a text field and the desktop grew one. The
 * file browser wanted a list and the desktop grew one. A board of eighty-one
 * little bevelled squares is not a widget anybody should add to a desktop, and
 * a zapp cannot draw it either, because a zapp never sees an lv_obj_t. ABI 0.7
 * is the answer to that: a grid of cells the desktop paints and the zapp fills
 * in, which is also a keypad, a palette, a character map and a calculator.
 *
 * The second thing it asked for is smaller and had been missing since 0.1.
 * There is a clock in the corner of Minesweeper, and until 0.7 a zapp could not
 * have one, because nothing in this ABI could wake a zapp up. Everything before
 * it ran inside a callback or did not run.
 *
 * What is NOT here, and is not an oversight:
 *
 *   - Chording: clicking both buttons on a satisfied number to open its
 *     neighbours. There is one button here -- see the note on long press in
 *     the desktop's chrome/cellgrid.c -- and the two-button gesture has no
 *     one-button spelling that is not also the two gestures we already use.
 *   - Question marks, the second state of the right-click cycle. They exist to
 *     mark a square you are unsure of, and they exist in the original mostly so
 *     that a mis-click has somewhere to go; with a long press as the flag
 *     gesture, a mis-click is much less likely and a third state in the cycle
 *     is a third long press to get back.
 *   - The high score table. It would need a name, which needs a prompt, which
 *     exists -- and a file, which exists -- and a date to stamp it with, which
 *     does not. Same three facts that took Time/Date out of Notepad.
 *   - Sound. There is no audio anywhere in this project.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/llext/symbol.h>

#include "mines.h"

const struct zd_host_api *ms_host;

/*
 * Per instance, not per image -- the rule notepad.h states at length. Two
 * Minesweepers share one copy of this file's .bss, so a file-scope board would
 * have the second window playing the first one's game.
 */
static struct ms_state states[MS_MAX_INSTANCES];

/** Gap between the panel and the board, in pixels. */
#define PANEL_GAP 2

/** Narrowest useful panel: three digits, a face, three digits. */
#define PANEL_MIN_COLS 7

static struct ms_state *claim_state(void)
{
	for (unsigned int i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		if (!states[i].used) {
			z_zero(&states[i], sizeof(states[i]));
			states[i].used = true;
			return &states[i];
		}
	}

	return NULL;
}

/* --- layout --------------------------------------------------------------------- */

/** Centre the panel and the board in whatever content area we now have. */
static void place(zd_zapp_ctx_t ctx, struct ms_state *st)
{
	int16_t pw;
	int16_t ph;
	int16_t bw;
	int16_t bh;

	if (st->cols == 0) {
		return;
	}

	ms_host->grid_measure(ctx, st->panel_cols, 1, &pw, &ph);
	ms_host->grid_measure(ctx, st->cols, st->rows, &bw, &bh);

	ms_host->grid_set_pos(ctx, st->panel,
			      (int16_t)(st->content_w > pw ? (st->content_w - pw) / 2 : 0),
			      0);
	ms_host->grid_set_pos(ctx, st->board,
			      (int16_t)(st->content_w > bw ? (st->content_w - bw) / 2 : 0),
			      (int16_t)(ph + PANEL_GAP));
}

/**
 * Resize the window so the content area is exactly @p w by @p h.
 *
 * window_set_geometry() takes the OUTER rectangle and a zapp is deliberately
 * never told how thick the chrome is, so the thickness is measured rather than
 * assumed: the difference between what window_get_geometry() reports and what
 * window_get_content_size() reports is the chrome, whatever this board's touch
 * slop has made of it.
 */
static void size_to_content(zd_zapp_ctx_t ctx, struct ms_state *st, int16_t w, int16_t h)
{
	struct zd_rect outer;
	int16_t chrome_w;
	int16_t chrome_h;
	int16_t most_w;
	int16_t most_h;
	int16_t cw;
	int16_t ch;

	if (ms_host->window_get_geometry(ctx, st->win, &outer) != 0 ||
	    ms_host->window_get_content_size(ctx, st->win, &cw, &ch) != 0) {
		return;
	}

	chrome_w = (int16_t)(outer.w - cw);
	chrome_h = (int16_t)(outer.h - ch);

	outer.w = (int16_t)(w + chrome_w);
	outer.h = (int16_t)(h + chrome_h);

	/* Growing a window that is not at the top-left can push its right or
	 * bottom edge off the screen, taking the resize grip with it. The
	 * desktop clamps the SIZE and leaves the position alone -- correctly,
	 * since a window may be moved off deliberately -- so pulling back is
	 * the caller's to do, and max_w/max_h plus the chrome is the edge.
	 */
	most_w = (int16_t)(st->max_w + chrome_w);
	most_h = (int16_t)(st->max_h + chrome_h);

	if (outer.x + outer.w > most_w) {
		outer.x = (int16_t)(most_w > outer.w ? most_w - outer.w : 0);
	}
	if (outer.y + outer.h > most_h) {
		outer.y = (int16_t)(most_h > outer.h ? most_h - outer.h : 0);
	}

	ms_host->window_set_geometry(ctx, st->win, &outer);

	/* Read back rather than assume. The desktop has both a floor and a
	 * ceiling on window size and says so; asking again is one call.
	 */
	ms_host->window_get_content_size(ctx, st->win, &st->content_w, &st->content_h);
}

/* --- starting a game -------------------------------------------------------------- */

static void announce(zd_zapp_ctx_t ctx, struct ms_state *st)
{
	char msg[64];
	uint32_t at;

	/* Permanent tracing. The shape of the board is decided at runtime from
	 * the room available, so "which game am I actually looking at" is not
	 * something a screenshot answers -- and it is the first thing to check
	 * when a board comes out the wrong size on a new panel.
	 */
	at = z_append(msg, 0, sizeof(msg), "mines: ");
	at = at != 0 ? z_append(msg, at, sizeof(msg), ms_levels[st->level].name) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), " ") : 0;
	at = at != 0 ? z_append_u32(msg, at, sizeof(msg), st->cols) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), "x") : 0;
	at = at != 0 ? z_append_u32(msg, at, sizeof(msg), st->rows) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), ", ") : 0;
	at = at != 0 ? z_append_u32(msg, at, sizeof(msg), st->mines) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), " mines") : 0;

	if (at != 0) {
		ms_host->log(ctx, 0, msg);
	}
}

/**
 * Lay out and start a game at @p level.
 *
 * The grids are reshaped BEFORE the state commits to the new dimensions. A
 * reshape can fail -- the shared cell pool is finite and another game may be
 * holding most of it -- and a state that says 16x16 over a grid still holding
 * 9x9 would index off the end of one of the two.
 */
static bool start_level(zd_zapp_ctx_t ctx, struct ms_state *st, uint8_t level)
{
	uint8_t fit_cols;
	uint8_t fit_rows;
	uint8_t cols;
	uint8_t rows;
	uint8_t panel_cols;
	uint16_t mines;
	int16_t pw;
	int16_t ph;
	int16_t bw;
	int16_t bh;

	/* Budget the panel at its narrowest first: it may end up wider than
	 * this, but only by being as wide as the board, which costs no height.
	 */
	ms_host->grid_measure(ctx, PANEL_MIN_COLS, 1, &pw, &ph);
	ms_host->grid_fit(ctx, st->max_w, (int16_t)(st->max_h - ph - PANEL_GAP), &fit_cols,
			  &fit_rows);

	ms_choose_shape(level, fit_cols, fit_rows, st->cap, &cols, &rows, &mines);
	if (cols == 0 || rows == 0) {
		ms_host->log(ctx, 0, "mines: no room for a board on this display");
		return false;
	}

	panel_cols = cols > PANEL_MIN_COLS ? cols : PANEL_MIN_COLS;

	if (st->board == NULL) {
		st->panel = ms_host->grid_create(ctx, st->win, 0, 0, panel_cols, 1);
		st->board = ms_host->grid_create(ctx, st->win, 0, 0, cols, rows);

		if (st->panel == NULL || st->board == NULL) {
			ms_host->log(ctx, 0, "mines: the desktop had no grid for us");
			return false;
		}
	} else {
		if (ms_host->grid_resize(ctx, st->board, cols, rows) != 0) {
			ms_host->log(ctx, 0, "mines: that board will not fit; keeping "
					     "the one we have");
			return false;
		}

		if (ms_host->grid_resize(ctx, st->panel, panel_cols, 1) != 0) {
			/*
			 * Half a reshape is worse than none: the state would
			 * say one size and the board would be another, and one
			 * of the two would be indexed off its end. Put the
			 * board back -- which cannot itself fail, because the
			 * block it wants was live a moment ago and the call
			 * that just failed took no cells.
			 */
			(void)ms_host->grid_resize(ctx, st->board, st->cols, st->rows);
			ms_host->log(ctx, 0, "mines: no room for that panel; keeping "
					     "the board we have");
			/* Reshaping blanked the cells; the model did not
			 * change, so redrawing from it restores the game.
			 */
			ms_draw_board(ctx, st);
			ms_draw_panel(ctx, st);
			return false;
		}
	}

	st->level = level;
	st->cols = cols;
	st->rows = rows;
	st->panel_cols = panel_cols;
	st->mines = mines;

	ms_reset(st);

	ms_host->grid_measure(ctx, panel_cols, 1, &pw, &ph);
	ms_host->grid_measure(ctx, cols, rows, &bw, &bh);
	size_to_content(ctx, st, (int16_t)(pw > bw ? pw : bw),
			(int16_t)(ph + PANEL_GAP + bh));

	place(ctx, st);
	ms_draw_board(ctx, st);
	ms_draw_panel(ctx, st);
	announce(ctx, st);

	return true;
}

/** Same level, new mines. What the face and F2 do. */
static void new_game(zd_zapp_ctx_t ctx, struct ms_state *st)
{
	ms_host->timer_stop(ctx, MS_TIMER_TICK);
	ms_reset(st);
	ms_draw_board(ctx, st);
	ms_draw_panel(ctx, st);
}

/* --- chrome ----------------------------------------------------------------------- */

static void build_menus(zd_zapp_ctx_t ctx, struct ms_state *st)
{
	zd_menu_t bar = ms_host->menubar_create(ctx, st->win);

	if (bar == NULL) {
		return; /* survivable: the face button still starts a new game */
	}

	st->game_menu = ms_host->menu_add_submenu(ctx, bar, "Game");
	ms_host->menu_add_item(ctx, st->game_menu, "New", MS_GAME_NEW);
	ms_host->menu_add_separator(ctx, st->game_menu);

	for (uint8_t i = 0; i < 3; i++) {
		ms_host->menu_add_item(ctx, st->game_menu, ms_levels[i].name,
				       (uint16_t)(MS_LEVEL_BASE + i));
	}

	ms_host->menu_add_separator(ctx, st->game_menu);
	ms_host->menu_add_item(ctx, st->game_menu, "Exit", MS_GAME_EXIT);
}

static void on_menu(zd_zapp_ctx_t ctx, struct ms_state *st, uint16_t id)
{
	switch (id) {
	case MS_GAME_NEW:
		new_game(ctx, st);
		break;

	case MS_GAME_EXIT:
		ms_host->window_close(ctx, st->win);
		break;

	default:
		if (id >= MS_LEVEL_BASE && id < MS_LEVEL_BASE + 3) {
			(void)start_level(ctx, st, (uint8_t)(id - MS_LEVEL_BASE));
		}
		break;
	}
}

/**
 * One line per move.
 *
 * Permanent, for the same reason the file browser logs which directory it is
 * showing: the state of a Minesweeper board is exactly the thing a console
 * cannot see and a screenshot cannot be diffed, and tools/qemu-drive.py is how
 * anything here gets checked without a hand on the mouse. Terse enough to be
 * worth having at INF -- a game is one line per click, and there are no clicks
 * unless somebody is playing.
 */
static void trace(zd_zapp_ctx_t ctx, struct ms_state *st, const char *verb, uint8_t col,
		  uint8_t row)
{
	char msg[64];
	uint32_t at;

	at = z_append(msg, 0, sizeof(msg), "mines: ");
	at = at != 0 ? z_append(msg, at, sizeof(msg), verb) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), " ") : 0;
	at = at != 0 ? z_append_u32(msg, at, sizeof(msg), col) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), ",") : 0;
	at = at != 0 ? z_append_u32(msg, at, sizeof(msg), row) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), " -> ") : 0;
	at = at != 0 ? z_append_u32(msg, at, sizeof(msg), st->opened) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), " open, ") : 0;
	at = at != 0 ? z_append_u32(msg, at, sizeof(msg), st->flags) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), " flagged, ") : 0;
	/* The clock as the panel is showing it, which is the only evidence from
	 * outside that ZD_EV_TIMER is arriving at all -- a headless run has no
	 * other way to see a counter tick.
	 */
	at = at != 0 ? z_append_u32(msg, at, sizeof(msg), st->shown_secs) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), "s") : 0;

	if (at != 0) {
		ms_host->log(ctx, 0, msg);
	}
}

static void on_cell(zd_zapp_ctx_t ctx, struct ms_state *st, const struct zd_event *ev)
{
	if (ev->grid.grid == st->panel) {
		/* Only the face. The counters are display, and a click on a
		 * blank filler cell should do nothing at all.
		 */
		if (ev->grid.col == (st->panel_cols - 1) / 2) {
			new_game(ctx, st);
		}
		return;
	}

	if (ev->grid.grid != st->board) {
		return;
	}

	if (ev->grid.action == ZD_GRID_SECONDARY) {
		/* One square changed, so redraw one square. The whole board is
		 * three hundred set_cell calls and this is one.
		 */
		if (ms_toggle_flag(st, ev->grid.col, ev->grid.row)) {
			ms_draw_cell(ctx, st, ev->grid.col, ev->grid.row);
			ms_draw_panel(ctx, st);
			trace(ctx, st, "flag", ev->grid.col, ev->grid.row);
		}
		return;
	}

	/*
	 * Rewriting the entire board from inside the dispatch of a click on it.
	 * This is the thing that would be a use-after-free with any of the
	 * desktop's other widgets and is simply an array write here -- see the
	 * header of chrome/cellgrid.c.
	 */
	if (ms_reveal(ctx, st, ev->grid.col, ev->grid.row)) {
		ms_draw_board(ctx, st);
		ms_draw_panel(ctx, st);
		trace(ctx, st, "open", ev->grid.col, ev->grid.row);
	}
}

/* --- lifecycle -------------------------------------------------------------------- */

static int mines_init(zd_zapp_ctx_t ctx, const struct zd_host_api *api)
{
	struct zd_window_desc desc = {
		.title = "Minesweeper",
		.geom = { 0, 0, 0, 0 },
	};
	struct zd_rect huge;
	struct ms_state *st;
	int cap;

	ms_host = api;

	st = claim_state();
	if (st == NULL) {
		return -1;
	}

	api->set_user_data(ctx, st);

	st->win = api->window_create(ctx, &desc);
	if (st->win == NULL) {
		st->used = false;
		return -1;
	}

	/* The bar first, so the content area it takes its height out of is
	 * already gone before anything is measured against it.
	 */
	build_menus(ctx, st);

	/*
	 * How much room is there? Ask for an absurd window; the desktop lowers
	 * it to the usable area, and reading back what arrived is the answer.
	 * See window_set_geometry() in the ABI header for why there is no
	 * screen-size call to use instead.
	 *
	 * Keeping the position the desktop cascaded us to, though. Sending
	 * (0,0) here is the same probe and it also stacks every Minesweeper in
	 * the top-left corner, which is exactly the tidiness the cascade
	 * exists to provide.
	 */
	if (api->window_get_geometry(ctx, st->win, &huge) != 0) {
		st->used = false;
		return -1;
	}
	huge.w = 4096;
	huge.h = 4096;
	api->window_set_geometry(ctx, st->win, &huge);

	if (api->window_get_content_size(ctx, st->win, &st->max_w, &st->max_h) != 0) {
		st->used = false;
		return -1;
	}
	st->content_w = st->max_w;
	st->content_h = st->max_h;

	cap = api->grid_get_capacity(ctx);
	st->cap = (uint16_t)(cap > 0 && cap < MS_MAX_CELLS ? cap : MS_MAX_CELLS);

	/* Seeded from the clock, because two games started in one boot should
	 * not be the same game. Coarse -- the loop's cadence is tens of
	 * milliseconds -- which is fine for a board and is why z_rand32 says
	 * plainly that it is not for anything that must be unguessable.
	 */
	st->rng = (uint32_t)api->uptime_ms();

	if (!start_level(ctx, st, 0)) {
		st->used = false;
		return -1;
	}

	api->log(ctx, 0, "Minesweeper ready");
	return 0;
}

static void mines_event(zd_zapp_ctx_t ctx, const struct zd_event *ev)
{
	struct ms_state *st = ms_host->get_user_data(ctx);

	if (st == NULL) {
		return;
	}

	/* ZD_EV_TIMER carries no window -- a timer belongs to the instance --
	 * so the usual "is this mine" guard has to come after the type check
	 * rather than before it. The ABI says so; this is what it looks like.
	 */
	if (ev->type == ZD_EV_TIMER) {
		if (ev->timer.id != MS_TIMER_TICK || st->phase != MS_PLAYING) {
			return;
		}

		/* Elapsed from uptime_ms(), not by counting ticks. The timer
		 * fires from the desktop loop, so it is late by however long
		 * the last repaint took, and a game that lost a second per
		 * cascade would be a stopwatch nobody could trust.
		 */
		/*
		 * The subtraction is 64-bit and the DIVISION MUST NOT BE.
		 *
		 * uptime_ms() returns an int64_t, so the obvious spelling --
		 * (now - started) / 1000 -- is a 64-bit divide, which is a
		 * native instruction on arm64 and a call to libgcc's __divdi3
		 * on 32-bit Xtensa. A zapp imports exactly one symbol, so that
		 * builds cleanly for the CoreS3 and then fails to load on it.
		 * `nm -D -u build-cores3/mines.llext` is what caught it, which
		 * is why CLAUDE.md says to run it.
		 *
		 * Narrowing first is safe and not a fudge: the delta is
		 * milliseconds since this game started, and 32 bits of those
		 * is forty-nine days.
		 */
		uint32_t elapsed = (uint32_t)(ms_host->uptime_ms() - st->started_ms);
		uint16_t secs = (uint16_t)(elapsed / 1000);

		if (secs != st->shown_secs) {
			st->shown_secs = secs;
			ms_draw_panel(ctx, st);
		}
		return;
	}

	if (ev->win != st->win) {
		return;
	}

	switch (ev->type) {
	case ZD_EV_GRID_CLICK:
		on_cell(ctx, st, ev);
		break;

	case ZD_EV_MENU:
		on_menu(ctx, st, ev->menu.id);
		break;

	case ZD_EV_RESIZED:
		/* The board keeps its shape and re-centres. Reshaping it would
		 * mean throwing away the game in progress because somebody
		 * nudged the grip, which is not a trade anyone would take.
		 */
		st->content_w = ev->resize.w;
		st->content_h = ev->resize.h;
		place(ctx, st);
		break;

	case ZD_EV_KEY:
		if (ev->key.code == ZD_KEY_F(2)) {
			new_game(ctx, st);
		}
		break;

	case ZD_EV_WINDOW_CLOSE_REQUEST:
		/* Nothing to save and nothing to argue about. A game that
		 * asked whether you were sure would be a game nobody closes.
		 */
		ms_host->window_close(ctx, st->win);
		break;

	default:
		break;
	}
}

static void mines_fini(zd_zapp_ctx_t ctx)
{
	struct ms_state *st = ms_host->get_user_data(ctx);

	if (st != NULL) {
		/* The desktop stops our timers for us at teardown -- it has to,
		 * because a timer outliving the code it dispatches into is a
		 * use-after-free. Stopping it here anyway costs one call and
		 * keeps the pairing visible in the zapp that made it.
		 */
		ms_host->timer_stop(ctx, MS_TIMER_TICK);
		st->used = false;
	}

	ms_host->log(ctx, 0, "Minesweeper closed");
}

struct zd_zapp_manifest zd_zapp_manifest = {
	.magic = ZD_ZAPP_MAGIC,
	.abi_major = ZD_ABI_MAJOR,
	.abi_minor = ZD_ABI_MINOR,
	.flags = 0, /* untrusted, and two at once is a feature */
	.name = "Minesweeper",
	.icon = NULL,
	.init = mines_init,
	.event = mines_event,
	.fini = mines_fini,
};
LL_EXTENSION_SYMBOL(zd_zapp_manifest);
