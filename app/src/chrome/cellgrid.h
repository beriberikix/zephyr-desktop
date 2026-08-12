/*
 * zephyr-desktop — a sheet of small bevelled cells, drawn as one object.
 *
 * The fourth shape of content the desktop knows how to draw, after the label,
 * the text field and the row list, and the one that exists because a zapp never
 * sees an lv_obj_t. A board, a keypad, a palette and a character map are all
 * the same thing: a rectangle of little squares, each with a bevel and a
 * character on it, that you can click. Nothing in the desktop could draw one.
 *
 * ONE lv_obj_t, NOT ONE PER CELL, and that is the whole design rather than an
 * optimisation. Eighty-one lv_obj_t would be eighty-one styles, eighty-one
 * layout entries and eighty-one event lists to show a Minesweeper board -- but
 * far more importantly, it would put this widget straight back into the hazard
 * CLAUDE.md opens with. A zapp answers a click on a cell by rewriting the
 * board, from a stack frame standing on the object that was clicked; with child
 * objects that is the deferred-reap problem for the sixth time, and the row
 * list only escaped it by keeping a model.
 *
 * Here there is nothing to defer, because there is nothing to destroy. The
 * cells are an array of eight-byte records; the picture is a draw callback over
 * that array. Setting every cell from inside a click handler touches no LVGL
 * object at all -- it writes memory and invalidates a rectangle. The model/view
 * rule the WM applies to stacking order and the row list applies to rows is
 * here a fact about the implementation rather than a discipline anyone has to
 * keep.
 *
 * THE COST IS DRAW TASKS, and it is worth stating because it is the one thing
 * that can go wrong. A cell is a background fill, two bevel rings of four
 * one-pixel rectangles each, and a label: ten LVGL draw tasks. A full 9x9 board
 * is therefore eight hundred of them in one repaint. They execute and are freed
 * as they are created -- lv_draw_finalize_task_creation() dispatches each one
 * -- so the memory does not pile up, but the time does. zd_cellgrid_set()
 * invalidates ONE CELL rather than the grid, so the common case (uncover a
 * square) repaints ten tasks and only a new game repaints eight hundred.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_CHROME_CELLGRID_H_
#define ZD_CHROME_CELLGRID_H_

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

struct zd_cellgrid;

/**
 * A cell was clicked.
 *
 * @param secondary the press was held rather than tapped. See the note on
 *                  LV_EVENT_LONG_PRESSED in cellgrid.c: exactly one of the two
 *                  is reported per gesture, never both.
 */
typedef void (*zd_cellgrid_cb_t)(void *user, uint8_t col, uint8_t row, bool secondary);

/**
 * @brief One cell's side, in pixels.
 *
 * Grows with CONFIG_ZD_TOUCH_SLOP_PX, like every other small control in this
 * desktop and for the reason stated three times in wm/wm.h: adjacent controls
 * must be BIGGER, not claim hit area they do not occupy. Cells are the most
 * adjacent controls there could be -- they share edges -- so giving them slop
 * instead of size would make every tap land on the neighbour LVGL happened to
 * add last.
 *
 * Not exported to zapps as a number. They ask zd_cellgrid_fit() and
 * zd_cellgrid_measure() instead, so nobody multiplies anything.
 */
int16_t zd_cellgrid_cell_size(void);

/** Most cells one grid may have. Reported through the ABI; never assumed. */
int zd_cellgrid_capacity(void);

/** Pixels a @p cols by @p rows grid occupies, border included. */
void zd_cellgrid_measure(uint8_t cols, uint8_t rows, int16_t *w, int16_t *h);

/** The inverse: the largest grid fitting in @p w by @p h. Either may be 0. */
void zd_cellgrid_fit(int16_t w, int16_t h, uint8_t *cols, uint8_t *rows);

/**
 * @brief Build a grid at @p x, @p y under @p parent.
 *
 * @return NULL if the table is full, the shared cell pool has no room, or
 *         cols*rows exceeds zd_cellgrid_capacity().
 */
struct zd_cellgrid *zd_cellgrid_create(lv_obj_t *parent, int16_t x, int16_t y,
				       uint8_t cols, uint8_t rows);
void zd_cellgrid_destroy(struct zd_cellgrid *g);
void zd_cellgrid_set_cb(struct zd_cellgrid *g, zd_cellgrid_cb_t cb, void *user);
void zd_cellgrid_set_pos(struct zd_cellgrid *g, int16_t x, int16_t y);

/** Reshape. Every cell is reset, as by zd_cellgrid_clear(). */
int zd_cellgrid_resize(struct zd_cellgrid *g, uint8_t cols, uint8_t rows);

/**
 * @brief Say what one cell looks like.
 *
 * @param text  copied, truncated at ZD_CELL_TEXT_MAX. NULL blanks the cell.
 * @param style ZD_CELL_RAISED, _SUNKEN or _FLAT.
 * @param rgb   0xRRGGBB for the text.
 */
int zd_cellgrid_set(struct zd_cellgrid *g, uint8_t col, uint8_t row, const char *text,
		    uint32_t style, uint32_t rgb);

/** Read a cell's style back out of the model. @return -EINVAL off the grid. */
int zd_cellgrid_style(const struct zd_cellgrid *g, uint8_t col, uint8_t row);

/** Every cell blank and raised. */
int zd_cellgrid_clear(struct zd_cellgrid *g);

uint8_t zd_cellgrid_cols(const struct zd_cellgrid *g);
uint8_t zd_cellgrid_rows(const struct zd_cellgrid *g);
lv_obj_t *zd_cellgrid_obj(const struct zd_cellgrid *g);

/**
 * @brief Which cell is at @p px, @p py relative to the grid's top-left?
 *
 * Exposed so the boot selftest can check the hit test without a pointer. The
 * arithmetic is the interesting part of this widget's input handling and the
 * part a board fragment's touch slop can break.
 *
 * @return true if the point landed on a cell rather than on the border.
 */
bool zd_cellgrid_hit(const struct zd_cellgrid *g, int32_t px, int32_t py, uint8_t *col,
		     uint8_t *row);

/** Live grids and cells taken from the shared pool, for leak assertions. */
uint32_t zd_cellgrid_live_count(void);
uint32_t zd_cellgrid_cells_used(void);

#endif /* ZD_CHROME_CELLGRID_H_ */
