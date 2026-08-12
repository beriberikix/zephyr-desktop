/*
 * zephyr-desktop — implementation of the cell grid.
 *
 * Read cellgrid.h first for why this is one object and not one per cell. What
 * follows is the part that only shows up down here.
 *
 * LONG PRESS IS THE SECOND BUTTON, AND IT ARRIVES BEFORE THE FIRST. A grid of
 * cells is the first thing this desktop has drawn that genuinely wants two
 * verbs on one cell -- uncover it, or mark it -- and there is no second button
 * to reach for. LVGL's pointer indev has one, the virtio tablet reports one,
 * and a fingertip has one. So a held press is the other verb, which is what
 * every touch program that ever needed a right-click settled on.
 *
 * The trap is the ordering, and it is the same shape as the row list's
 * DOUBLE_CLICKED-before-CLICKED: LV_EVENT_LONG_PRESSED fires while the button
 * is still down, and LV_EVENT_CLICKED then fires anyway when it comes up. A
 * naive reading marks the cell and immediately uncovers it -- which in
 * Minesweeper means flagging a mine and then stepping on it. One bool fixes it:
 * PRESSED clears it, LONG_PRESSED sets it, CLICKED honours it and returns. The
 * contract a zapp gets is that a gesture produces exactly one event.
 *
 * Note what is deliberately NOT here: press feedback. Windows drew the cell
 * under your finger sunken while you held it, which needs an invalidate per
 * pointer sample and a fifth piece of state, and buys nothing on a panel where
 * your thumb is over the cell anyway. Said out loud rather than left as a
 * question, because it is the first thing anyone will notice is missing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cellgrid.h"
#include "theme.h"

#include <zd/zapp_abi.h>

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/*
 * A cell's side, and the border around the sheet of them.
 *
 * 16 is Minesweeper's number and Win95's toolbar-button number, and it is what
 * the slop is added to for the reason wm/wm.h gives at length. BORDER is the
 * two-pixel sunken ring around the whole grid plus one pixel of face, so the
 * outermost cells' own bevels do not touch it.
 */
#define CELL_SZ (16 + CONFIG_ZD_TOUCH_SLOP_PX)
#define BORDER  3

/** Font of the thing written on a cell. One character, so it must be small. */
#define CELL_FONT (&lv_font_montserrat_12)

struct cell {
	char text[ZD_CELL_TEXT_MAX];
	uint8_t style;
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

BUILD_ASSERT(sizeof(struct cell) == 8, "a cell was meant to be eight bytes");

struct zd_cellgrid {
	lv_obj_t *view;

	/* Where this grid's cells live in the shared pool. len is exactly
	 * cols*rows; a len of 0 means the block is not reserved, which is how
	 * resize releases its old space to the allocator without losing it.
	 */
	uint32_t off;
	uint32_t len;

	uint8_t cols;
	uint8_t rows;

	bool used;
	/** A SECONDARY already went out for the press now in progress. */
	bool long_fired;

	zd_cellgrid_cb_t cb;
	void *user;
};

/*
 * One pool of cells for the whole desktop, exactly as the row list keeps one
 * pool of rows and for the same reason: four grids each able to hold a full
 * board would reserve most of a CoreS3's spare .bss to draw a nine-by-nine.
 *
 * Unlike rows, cells are allocated a whole grid at a time and are indexed
 * arithmetically, so this is a block allocator over one array rather than a
 * free list. With at most CONFIG_ZD_MAX_GRIDS live blocks the search below is
 * exhaustive and obviously terminating, which is the only kind of allocator
 * worth having in a desktop shell.
 */
static struct cell pool[CONFIG_ZD_GRID_CELLS_TOTAL];
static struct zd_cellgrid grids[CONFIG_ZD_MAX_GRIDS];
static uint32_t live_grids;

/* --- the pool ------------------------------------------------------------------- */

/**
 * Lowest offset at which @p need cells do not overlap a live block.
 *
 * Bumps past each conflict and starts again. It terminates because the offset
 * strictly increases on every pass and is bounded by the pool, and it finds
 * genuine gaps rather than only appending: given blocks at [0,10) and [20,30)
 * and a request for 5, it stops at 10.
 */
static bool block_alloc(uint32_t need, uint32_t *out)
{
	uint32_t at = 0;
	bool moved;

	if (need == 0) {
		return false;
	}

	do {
		moved = false;

		for (size_t i = 0; i < ARRAY_SIZE(grids); i++) {
			const struct zd_cellgrid *g = &grids[i];

			if (!g->used || g->len == 0) {
				continue;
			}
			if (at < g->off + g->len && g->off < at + need) {
				at = g->off + g->len;
				moved = true;
			}
		}
	} while (moved);

	if (at + need > ARRAY_SIZE(pool)) {
		return false;
	}

	*out = at;
	return true;
}

static struct cell *cell_at(const struct zd_cellgrid *g, uint8_t col, uint8_t row)
{
	if (col >= g->cols || row >= g->rows) {
		return NULL;
	}

	return &pool[g->off + (uint32_t)row * g->cols + col];
}

static void blank(struct zd_cellgrid *g)
{
	for (uint32_t i = 0; i < g->len; i++) {
		pool[g->off + i] = (struct cell){ .style = ZD_CELL_RAISED };
	}
}

/* --- geometry --------------------------------------------------------------------- */

int16_t zd_cellgrid_cell_size(void)
{
	return CELL_SZ;
}

int zd_cellgrid_capacity(void)
{
	return CONFIG_ZD_GRID_MAX_CELLS;
}

void zd_cellgrid_measure(uint8_t cols, uint8_t rows, int16_t *w, int16_t *h)
{
	if (w != NULL) {
		*w = (int16_t)(cols * CELL_SZ + 2 * BORDER);
	}
	if (h != NULL) {
		*h = (int16_t)(rows * CELL_SZ + 2 * BORDER);
	}
}

void zd_cellgrid_fit(int16_t w, int16_t h, uint8_t *cols, uint8_t *rows)
{
	int32_t across = (w - 2 * BORDER) / CELL_SZ;
	int32_t down = (h - 2 * BORDER) / CELL_SZ;

	if (cols != NULL) {
		*cols = (uint8_t)CLAMP(across, 0, 255);
	}
	if (rows != NULL) {
		*rows = (uint8_t)CLAMP(down, 0, 255);
	}
}

bool zd_cellgrid_hit(const struct zd_cellgrid *g, int32_t px, int32_t py, uint8_t *col,
		     uint8_t *row)
{
	int32_t c;
	int32_t r;

	if (g == NULL || !g->used) {
		return false;
	}

	px -= BORDER;
	py -= BORDER;

	if (px < 0 || py < 0) {
		return false;
	}

	c = px / CELL_SZ;
	r = py / CELL_SZ;

	if (c >= g->cols || r >= g->rows) {
		return false;
	}

	*col = (uint8_t)c;
	*row = (uint8_t)r;
	return true;
}

/** The absolute rectangle of one cell, for drawing and for invalidating. */
static bool cell_area(const struct zd_cellgrid *g, uint8_t col, uint8_t row,
		      lv_area_t *out)
{
	lv_area_t coords;

	lv_obj_get_coords(g->view, &coords);

	if (lv_area_get_width(&coords) <= 0) {
		return false; /* not laid out yet; the first paint covers it */
	}

	out->x1 = coords.x1 + BORDER + col * CELL_SZ;
	out->y1 = coords.y1 + BORDER + row * CELL_SZ;
	out->x2 = out->x1 + CELL_SZ - 1;
	out->y2 = out->y1 + CELL_SZ - 1;
	return true;
}

/* --- painting --------------------------------------------------------------------- */

static void grid_draw_cb(lv_event_t *e)
{
	struct zd_cellgrid *g = lv_event_get_user_data(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_obj_t *obj = lv_event_get_target_obj(e);
	lv_draw_rect_dsc_t face;
	lv_draw_label_dsc_t txt;
	int32_t line_h = lv_font_get_line_height(CELL_FONT);
	lv_area_t coords;

	if (g == NULL || !g->used) {
		return;
	}

	lv_obj_get_coords(obj, &coords);

	lv_draw_rect_dsc_init(&face);
	face.bg_color = lv_color_hex(ZD_C_FACE);
	face.bg_opa = LV_OPA_COVER;
	face.border_width = 0;
	face.radius = 0;

	lv_draw_label_dsc_init(&txt);
	txt.font = CELL_FONT;
	txt.align = LV_TEXT_ALIGN_CENTER;
	txt.opa = LV_OPA_COVER;

	for (uint8_t row = 0; row < g->rows; row++) {
		for (uint8_t col = 0; col < g->cols; col++) {
			const struct cell *c = &pool[g->off + (uint32_t)row * g->cols + col];
			lv_area_t area;

			area.x1 = coords.x1 + BORDER + col * CELL_SZ;
			area.y1 = coords.y1 + BORDER + row * CELL_SZ;
			area.x2 = area.x1 + CELL_SZ - 1;
			area.y2 = area.y1 + CELL_SZ - 1;

			lv_draw_rect(layer, &face, &area);

			if (c->style == ZD_CELL_RAISED) {
				zd_bevel_draw(layer, &area, ZD_BEVEL_OUT);
			} else if (c->style == ZD_CELL_SUNKEN) {
				zd_bevel_draw(layer, &area, ZD_BEVEL_IN);
			}

			if (c->text[0] == '\0') {
				continue;
			}

			/* Centred by hand vertically: lv_draw_label only aligns
			 * horizontally, and the cell is barely taller than a
			 * line, so being a pixel out is visible.
			 */
			txt.text = c->text;
			txt.color = lv_color_make(c->r, c->g, c->b);
			area.y1 += (CELL_SZ - line_h) / 2;
			lv_draw_label(layer, &txt, &area);
		}
	}
}

/* --- input ------------------------------------------------------------------------ */

/** Where the pointer is, relative to the grid's top-left. */
static bool pointer_cell(struct zd_cellgrid *g, lv_event_t *e, uint8_t *col, uint8_t *row)
{
	lv_indev_t *indev = lv_event_get_indev(e);
	lv_area_t coords;
	lv_point_t p;

	if (indev == NULL) {
		return false;
	}

	lv_indev_get_point(indev, &p);
	lv_obj_get_coords(g->view, &coords);

	return zd_cellgrid_hit(g, p.x - coords.x1, p.y - coords.y1, col, row);
}

static void grid_pressed(lv_event_t *e)
{
	struct zd_cellgrid *g = lv_event_get_user_data(e);

	if (g != NULL && g->used) {
		g->long_fired = false;
	}
}

static void grid_long_pressed(lv_event_t *e)
{
	struct zd_cellgrid *g = lv_event_get_user_data(e);
	uint8_t col;
	uint8_t row;

	if (g == NULL || !g->used || g->cb == NULL) {
		return;
	}

	if (!pointer_cell(g, e, &col, &row)) {
		return;
	}

	/* Set before the callback, not after: the callback may rewrite the whole
	 * grid, and the CLICKED that is still coming has to be suppressed even
	 * if it does.
	 */
	g->long_fired = true;
	g->cb(g->user, col, row, true);
}

static void grid_clicked(lv_event_t *e)
{
	struct zd_cellgrid *g = lv_event_get_user_data(e);
	uint8_t col;
	uint8_t row;

	if (g == NULL || !g->used) {
		return;
	}

	/* One gesture, one event. See the file header. */
	if (g->long_fired) {
		g->long_fired = false;
		lv_event_stop_bubbling(e);
		return;
	}

	if (g->cb != NULL && pointer_cell(g, e, &col, &row)) {
		g->cb(g->user, col, row, false);
	}

	/* The press already bubbled and raised the window, which is what
	 * bubbling is for here. Stop the click, so a zapp is not told both
	 * "cell 3,4" and "you were clicked at (52,71)" for one tap.
	 */
	lv_event_stop_bubbling(e);
}

/*
 * Released by LVGL, not by us, exactly as the row list and the text widget are:
 * a grid dies either because its owner destroyed it or because the window it
 * lives in was reaped and lv_obj_delete() took the subtree. One path covers
 * both, so there is no way to close a window and leak a block of cells.
 */
static void view_deleted(lv_event_t *e)
{
	struct zd_cellgrid *g = lv_event_get_user_data(e);

	if (g == NULL || !g->used) {
		return;
	}

	g->view = NULL;
	g->used = false;
	g->len = 0;
	g->cb = NULL;
	live_grids--;
}

/* --- lifecycle -------------------------------------------------------------------- */

struct zd_cellgrid *zd_cellgrid_create(lv_obj_t *parent, int16_t x, int16_t y,
				       uint8_t cols, uint8_t rows)
{
	struct zd_cellgrid *g = NULL;
	uint32_t need = (uint32_t)cols * rows;
	uint32_t off;
	int16_t w;
	int16_t h;

	if (cols == 0 || rows == 0) {
		return NULL;
	}

	if (need > CONFIG_ZD_GRID_MAX_CELLS) {
		LOG_WRN("grid of %ux%u is over the %d cell cap", cols, rows,
			CONFIG_ZD_GRID_MAX_CELLS);
		return NULL;
	}

	for (size_t i = 0; i < ARRAY_SIZE(grids); i++) {
		if (!grids[i].used) {
			g = &grids[i];
			break;
		}
	}

	if (g == NULL) {
		LOG_WRN("grid table full (%d)", CONFIG_ZD_MAX_GRIDS);
		return NULL;
	}

	if (!block_alloc(need, &off)) {
		LOG_WRN("grid cell pool exhausted (%d) asking for %u",
			CONFIG_ZD_GRID_CELLS_TOTAL, need);
		return NULL;
	}

	g->view = lv_obj_create(parent);
	lv_obj_remove_style_all(g->view);
	lv_obj_add_style(g->view, &zd_style_face, LV_PART_MAIN);

	zd_cellgrid_measure(cols, rows, &w, &h);
	lv_obj_set_pos(g->view, x, y);
	lv_obj_set_size(g->view, w, h);

	lv_obj_add_flag(g->view, LV_OBJ_FLAG_CLICKABLE);
	/* So a press inside still raises and focuses the window, exactly as it
	 * does for a label or a list. The click itself is stopped in
	 * grid_clicked().
	 */
	lv_obj_add_flag(g->view, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_remove_flag(g->view, LV_OBJ_FLAG_SCROLLABLE);

	zd_bevel_attach(g->view, ZD_BEVEL_IN);

	g->off = off;
	g->len = need;
	g->cols = cols;
	g->rows = rows;
	g->used = true;
	g->long_fired = false;
	g->cb = NULL;
	g->user = NULL;

	blank(g);
	live_grids++;

	lv_obj_add_event_cb(g->view, grid_draw_cb, LV_EVENT_DRAW_POST, g);
	lv_obj_add_event_cb(g->view, grid_pressed, LV_EVENT_PRESSED, g);
	lv_obj_add_event_cb(g->view, grid_long_pressed, LV_EVENT_LONG_PRESSED, g);
	lv_obj_add_event_cb(g->view, grid_clicked, LV_EVENT_CLICKED, g);
	lv_obj_add_event_cb(g->view, view_deleted, LV_EVENT_DELETE, g);

	return g;
}

void zd_cellgrid_destroy(struct zd_cellgrid *g)
{
	if (g != NULL && g->used) {
		/* The DELETE handler does the bookkeeping, so this takes the
		 * same path a window close takes.
		 */
		lv_obj_delete(g->view);
	}
}

void zd_cellgrid_set_cb(struct zd_cellgrid *g, zd_cellgrid_cb_t cb, void *user)
{
	if (g != NULL) {
		g->cb = cb;
		g->user = user;
	}
}

void zd_cellgrid_set_pos(struct zd_cellgrid *g, int16_t x, int16_t y)
{
	if (g != NULL && g->used) {
		lv_obj_set_pos(g->view, x, y);
	}
}

int zd_cellgrid_resize(struct zd_cellgrid *g, uint8_t cols, uint8_t rows)
{
	uint32_t need = (uint32_t)cols * rows;
	uint32_t was_off;
	uint32_t was_len;
	int16_t w;
	int16_t h;

	if (g == NULL || !g->used || cols == 0 || rows == 0) {
		return -EINVAL;
	}

	if (need > CONFIG_ZD_GRID_MAX_CELLS) {
		return -EINVAL;
	}

	was_off = g->off;
	was_len = g->len;

	/* Release our own block first so the allocator can hand the same space
	 * straight back, which is what happens every time a grid is reshaped
	 * smaller or the same. Restored untouched if there is no room.
	 */
	g->len = 0;

	if (!block_alloc(need, &g->off)) {
		g->off = was_off;
		g->len = was_len;
		return -ENOSPC;
	}

	g->len = need;
	g->cols = cols;
	g->rows = rows;
	blank(g);

	zd_cellgrid_measure(cols, rows, &w, &h);
	lv_obj_set_size(g->view, w, h);
	lv_obj_invalidate(g->view);

	return 0;
}

/* --- the model -------------------------------------------------------------------- */

int zd_cellgrid_set(struct zd_cellgrid *g, uint8_t col, uint8_t row, const char *text,
		    uint32_t style, uint32_t rgb)
{
	struct cell *c;
	lv_area_t area;

	if (g == NULL || !g->used || style > ZD_CELL_FLAT) {
		return -EINVAL;
	}

	c = cell_at(g, col, row);
	if (c == NULL) {
		return -EINVAL;
	}

	if (text == NULL) {
		c->text[0] = '\0';
	} else {
		(void)strncpy(c->text, text, sizeof(c->text) - 1);
		c->text[sizeof(c->text) - 1] = '\0';
	}

	c->style = (uint8_t)style;
	c->r = (uint8_t)(rgb >> 16);
	c->g = (uint8_t)(rgb >> 8);
	c->b = (uint8_t)rgb;

	/* One cell, not the grid. Uncovering a square is ten draw tasks this
	 * way and eight hundred the other; see cellgrid.h.
	 */
	if (cell_area(g, col, row, &area)) {
		lv_obj_invalidate_area(g->view, &area);
	} else {
		lv_obj_invalidate(g->view);
	}

	return 0;
}

int zd_cellgrid_style(const struct zd_cellgrid *g, uint8_t col, uint8_t row)
{
	const struct cell *c;

	if (g == NULL || !g->used) {
		return -EINVAL;
	}

	c = cell_at(g, col, row);
	return c != NULL ? (int)c->style : -EINVAL;
}

int zd_cellgrid_clear(struct zd_cellgrid *g)
{
	if (g == NULL || !g->used) {
		return -EINVAL;
	}

	blank(g);
	lv_obj_invalidate(g->view);
	return 0;
}

uint8_t zd_cellgrid_cols(const struct zd_cellgrid *g)
{
	return (g != NULL && g->used) ? g->cols : 0;
}

uint8_t zd_cellgrid_rows(const struct zd_cellgrid *g)
{
	return (g != NULL && g->used) ? g->rows : 0;
}

lv_obj_t *zd_cellgrid_obj(const struct zd_cellgrid *g)
{
	return (g != NULL && g->used) ? g->view : NULL;
}

uint32_t zd_cellgrid_live_count(void)
{
	return live_grids;
}

uint32_t zd_cellgrid_cells_used(void)
{
	uint32_t total = 0;

	for (size_t i = 0; i < ARRAY_SIZE(grids); i++) {
		if (grids[i].used) {
			total += grids[i].len;
		}
	}

	return total;
}
