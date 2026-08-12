/*
 * zephyr-desktop — a scrolling column of selectable rows.
 *
 * Desktop-internal and handle-free: this file knows nothing about zapps, the
 * ABI, or the handle registry. host/list_api.c layers those on top, exactly as
 * it does for lv_textarea, and shell/dialog.c uses it raw for the file picker.
 * Two customers is the whole reason it is a separate file.
 *
 * THE LIST IS A MODEL, NOT A PICTURE, and that is the load-bearing idea here.
 *
 * zd_rowlist_clear() and zd_rowlist_add() change what the list *is*, at once:
 * count and selection answer from the model the instant you call them. The
 * rows on screen catch up when zd_rowlist_reap() runs, one desktop loop later.
 *
 * There are three reasons it is built that way, and only the first is the one
 * you would guess:
 *
 * ONE. Rebuilding a list from inside the dispatch of a row in that list -- what
 * "enter this directory" means -- would lv_obj_clean() the parent of the object
 * whose callback is still on the stack. CLAUDE.md's central rule, met for a
 * fifth time. Deferring the rebuild is the standing answer.
 *
 * TWO. It removes a bug class rather than dodging one. The picker this replaces
 * kept each row's filename alive by reading it back out of the LVGL label,
 * because the label owned the only copy. That is LVGL being the model, and it
 * is why "give me row 3's text" was not a question it could answer.
 *
 * THREE. It separates the two things that must not be interleaved on the
 * CoreS3: the caller fills the model from a readdir loop under the bus arbiter
 * and touches no LVGL, and the reap builds widgets and touches no filesystem.
 * The old picker held the arbiter across lv_obj_create(), which bus_arb.h says
 * never to do.
 *
 * None of this is new law. It is the rule the WM already applies to z-order --
 * the sys_dlist_t is the truth and LVGL is a projection re-applied by
 * zd_wm_restack() -- pointed at a different widget.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_CHROME_ROWLIST_H_
#define ZD_CHROME_ROWLIST_H_

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

struct zd_rowlist;

/**
 * @brief Told that the selection moved, or that a row was activated.
 *
 * Called from LVGL dispatch, so the usual rule applies: do not destroy anything
 * from in here. Rebuilding the list that called you is fine and is the point.
 */
typedef void (*zd_rowlist_cb_t)(void *user, int32_t index, uint16_t id);

/** Row height, so a caller can decide how many will fit before creating one. */
int16_t zd_rowlist_row_h(void);

/** @return NULL if the table is full. */
struct zd_rowlist *zd_rowlist_create(lv_obj_t *parent, int16_t x, int16_t y, int16_t w,
				     int16_t h);
void zd_rowlist_destroy(struct zd_rowlist *rl);

void zd_rowlist_set_cb(struct zd_rowlist *rl, zd_rowlist_cb_t on_select,
		       zd_rowlist_cb_t on_activate, void *user);
void zd_rowlist_set_geometry(struct zd_rowlist *rl, int16_t x, int16_t y, int16_t w,
			     int16_t h);

/** Empty it. The selection is cleared, never preserved. */
void zd_rowlist_clear(struct zd_rowlist *rl);
/** @return the new row's index, or -ENOSPC. */
int zd_rowlist_add(struct zd_rowlist *rl, const char *text, uint16_t id);

int zd_rowlist_count(const struct zd_rowlist *rl);
/** Most rows one list will hold. Nothing about a particular list. */
int zd_rowlist_capacity(void);
/** @return the selected row, or -1. */
int zd_rowlist_selected(const struct zd_rowlist *rl);
/** Select a row and scroll it into view. -1 clears. Fires no callback. */
int zd_rowlist_select(struct zd_rowlist *rl, int32_t index);
int zd_rowlist_item_id(const struct zd_rowlist *rl, int32_t index);
/** Copies whole or returns -ENOSPC. A half-copied filename opens the wrong file. */
int zd_rowlist_item_text(const struct zd_rowlist *rl, int32_t index, char *buf,
			 uint32_t len);

/**
 * @brief Offer a key to a list.
 *
 * Claims the navigation keys and Enter, and nothing else -- a list is not a
 * text field and has no business swallowing letters.
 *
 * @return true if it was used.
 */
bool zd_rowlist_key(struct zd_rowlist *rl, uint32_t code, uint16_t mods);

/** The container, for callers that track focus or need to hide it. */
lv_obj_t *zd_rowlist_obj(const struct zd_rowlist *rl);

/** Rebuild every dirty list's rows. Call from the desktop loop, never inside it. */
void zd_rowlist_reap(void);

/** Live lists, for leak assertions. */
uint32_t zd_rowlist_live_count(void);
/** Rows taken from the shared pool, for leak assertions. */
uint32_t zd_rowlist_items_used(void);

#endif /* ZD_CHROME_ROWLIST_H_ */
