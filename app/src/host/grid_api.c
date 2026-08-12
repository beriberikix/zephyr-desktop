/*
 * zephyr-desktop — implementation of the zapp-facing cell grid.
 *
 * Thin, like list_api.c and for the same reason: everything hard about a grid
 * -- one object drawing a hundred cells, the shared block pool, the long-press
 * ordering -- is in chrome/cellgrid.c. What is left here is a
 * generation-counted handle with an owner check, and a way for a cell click to
 * become an event on the other side of the ABI.
 *
 * There is one thing this layer does NOT have to do, and it is worth naming
 * because every other widget here has it. There is no deferred rebuild and no
 * dirty flag: a zapp answering ZD_EV_GRID_CLICK by rewriting every cell is
 * writing an array, and the grid it is writing has no child objects for the
 * callback to be standing on. list_api.c needs a paragraph explaining why the
 * same thing is safe there. Here there is nothing to explain.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "grid_api.h"
#include "../chrome/cellgrid.h"
#include "../loader/zapp_instance.h"
#include "../wm/handle.h"
#include "../wm/wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

struct grid_rec {
	struct zd_cellgrid *cg;
	struct zd_client *client;
	struct zd_zapp_instance *owner;
	uintptr_t handle;
	bool used;
};

static struct grid_rec recs[CONFIG_ZD_MAX_GRIDS];
static uint32_t live_grids;

static struct grid_rec *rec_of(struct zd_zapp_instance *owner, uintptr_t handle)
{
	return zd_handle_deref(handle, ZD_HANDLE_GRID, owner);
}

/* --- events out ------------------------------------------------------------------ */

static void on_cell(void *user, uint8_t col, uint8_t row, bool secondary)
{
	struct grid_rec *rec = user;
	struct zd_event ev = {
		.type = ZD_EV_GRID_CLICK,
		.win = (zd_window_t)rec->client->handle,
		.grid = {
			.grid = (zd_grid_t)rec->handle,
			.col = col,
			.row = row,
			.action = secondary ? ZD_GRID_SECONDARY : ZD_GRID_PRIMARY,
		},
	};

	if (rec->owner == NULL || rec->client->handle == 0) {
		return;
	}

	zd_zapp_dispatch(rec->owner, &ev);
}

/* --- lifecycle -------------------------------------------------------------------- */

/*
 * Released by LVGL, not by us -- the arrangement text_api.c and list_api.c both
 * use. A grid dies either because the zapp destroyed it or because the window
 * was reaped and lv_obj_delete() took the subtree; hanging teardown on the
 * object's deletion covers both with one path.
 */
static void view_deleted(lv_event_t *e)
{
	struct grid_rec *rec = lv_event_get_user_data(e);

	if (rec == NULL || !rec->used) {
		return;
	}

	zd_handle_free(rec->handle);
	rec->used = false;
	rec->cg = NULL;
	live_grids--;
}

uintptr_t zd_grid_create(struct zd_zapp_instance *owner, struct zd_client *client,
			 int16_t x, int16_t y, uint8_t cols, uint8_t rows)
{
	struct grid_rec *rec = NULL;
	struct zd_cellgrid *cg;

	for (size_t i = 0; i < ARRAY_SIZE(recs); i++) {
		if (!recs[i].used) {
			rec = &recs[i];
			break;
		}
	}

	if (rec == NULL) {
		LOG_WRN("grid widget table full (%d)", CONFIG_ZD_MAX_GRIDS);
		return 0;
	}

	cg = zd_cellgrid_create(client->content, x, y, cols, rows);
	if (cg == NULL) {
		return 0;
	}

	rec->cg = cg;
	rec->client = client;
	rec->owner = owner;
	rec->used = true;

	rec->handle = zd_handle_alloc(ZD_HANDLE_GRID, rec, owner);
	if (rec->handle == 0) {
		rec->used = false;
		zd_cellgrid_destroy(cg);
		return 0;
	}

	live_grids++;

	zd_cellgrid_set_cb(cg, on_cell, rec);
	lv_obj_add_event_cb(zd_cellgrid_obj(cg), view_deleted, LV_EVENT_DELETE, rec);

	return rec->handle;
}

void zd_grid_destroy(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct grid_rec *rec = rec_of(owner, handle);

	if (rec != NULL) {
		zd_cellgrid_destroy(rec->cg);
	}
}

int zd_grid_set_pos(struct zd_zapp_instance *owner, uintptr_t handle, int16_t x, int16_t y)
{
	struct grid_rec *rec = rec_of(owner, handle);

	if (rec == NULL) {
		return -EINVAL;
	}

	zd_cellgrid_set_pos(rec->cg, x, y);
	return 0;
}

int zd_grid_resize(struct zd_zapp_instance *owner, uintptr_t handle, uint8_t cols,
		   uint8_t rows)
{
	struct grid_rec *rec = rec_of(owner, handle);

	return rec != NULL ? zd_cellgrid_resize(rec->cg, cols, rows) : -EINVAL;
}

int zd_grid_set_cell(struct zd_zapp_instance *owner, uintptr_t handle, uint8_t col,
		     uint8_t row, const char *text, uint32_t style, uint32_t rgb)
{
	struct grid_rec *rec = rec_of(owner, handle);

	return rec != NULL ? zd_cellgrid_set(rec->cg, col, row, text, style, rgb) : -EINVAL;
}

int zd_grid_clear(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct grid_rec *rec = rec_of(owner, handle);

	return rec != NULL ? zd_cellgrid_clear(rec->cg) : -EINVAL;
}

uint32_t zd_grid_live_count(void)
{
	return live_grids;
}
