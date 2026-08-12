/*
 * zephyr-desktop — implementation of the scrolling row list.
 *
 * Read rowlist.h first for why the model and the view are separate. What
 * follows is the part that only shows up in the implementation.
 *
 * DOUBLE-CLICK IS LVGL'S, NOT OURS. lv_indev already tracks click streaks and
 * emits LV_EVENT_DOUBLE_CLICKED, with better thresholds than a hand-rolled
 * timer would have picked: the window is indev->long_press_time (400 ms) and
 * the movement tolerance is indev->scroll_limit -- the same number that decides
 * whether the gesture was a scroll. That coupling is exactly right. A finger
 * that travelled far enough to scroll the list was scrolling it, and calling
 * that a double-click would be wrong. There is deliberately no Kconfig for the
 * interval: it is the same variable as the long-press time, so a knob here
 * would silently move LV_EVENT_LONG_PRESSED too.
 *
 * AND THE ORDERING IS A TRAP. LVGL sends DOUBLE_CLICKED *before* CLICKED, to
 * the same object, in one release (lv_indev.c, indev_proc_release). So the
 * sequence when a directory row is double-clicked is: activate fires, the owner
 * rebuilds the model, and then CLICKED arrives at the same still-alive row --
 * an index into a listing that no longer exists. Two guards, both needed and
 * both cheap: clearing hides the container at once so no *new* press can reach
 * a stale row, and every row callback re-validates against the dirty flag
 * before it does anything, the way tasklist.c re-validates a pooled client
 * pointer on every click.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "rowlist.h"
#include "theme.h"

#include <zd/zapp_abi.h>

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/*
 * Contiguous rows, so they are made taller rather than given hit area they do
 * not occupy. Third statement of the rule in this codebase; see wm/wm.h. The
 * same arithmetic the file picker used, so the picker looks unchanged when it
 * moves onto this in L3.
 */
#define ROW_H     (16 + CONFIG_ZD_TOUCH_SLOP_PX)
#define ROW_PAD_X 4
#define LABEL_Y   ((ROW_H - 12) / 2)

#define NO_ITEM (-1)

struct row_item {
	char text[ZD_NAME_MAX];
	uint16_t id;
	int16_t next; /**< index into items[], or NO_ITEM */
};

struct zd_rowlist {
	lv_obj_t *view;

	int16_t head;
	int16_t tail;
	int16_t count;
	int16_t selected;

	bool dirty;
	bool used;

	zd_rowlist_cb_t on_select;
	zd_rowlist_cb_t on_activate;
	void *user;
};

/*
 * One pool of rows for the whole desktop, not an array per list.
 *
 * Four lists that could each hold ZD_LIST_MAX_ITEMS would reserve twenty
 * kilobytes of .bss to show a directory of nine files, and on the CoreS3 that
 * is internal RAM. Same shape as the open-file table: one bounded resource plus
 * a per-list cap, so a runaway list hits a wall that names itself without being
 * able to take everyone else's rows with it.
 */
static struct row_item items[CONFIG_ZD_LIST_ITEMS_TOTAL];
static int16_t free_head;
static bool pool_ready;
static uint32_t items_used;

static struct zd_rowlist lists[CONFIG_ZD_MAX_LISTS];
static uint32_t live_lists;

/* --- the pool ------------------------------------------------------------------ */

static void pool_init(void)
{
	for (int16_t i = 0; i < (int16_t)ARRAY_SIZE(items); i++) {
		items[i].next = (i + 1 < (int16_t)ARRAY_SIZE(items)) ? (int16_t)(i + 1)
								     : NO_ITEM;
	}

	free_head = ARRAY_SIZE(items) > 0 ? 0 : NO_ITEM;
	pool_ready = true;
}

static int16_t item_alloc(void)
{
	int16_t index;

	if (!pool_ready) {
		pool_init();
	}

	index = free_head;
	if (index == NO_ITEM) {
		return NO_ITEM;
	}

	free_head = items[index].next;
	items[index].next = NO_ITEM;
	items_used++;
	return index;
}

static void chain_release(struct zd_rowlist *rl)
{
	int16_t index = rl->head;

	while (index != NO_ITEM) {
		int16_t next = items[index].next;

		items[index].next = free_head;
		free_head = index;
		items_used--;
		index = next;
	}

	rl->head = NO_ITEM;
	rl->tail = NO_ITEM;
	rl->count = 0;
}

static struct row_item *item_at(const struct zd_rowlist *rl, int32_t index)
{
	int16_t at = rl->head;

	if (index < 0 || index >= rl->count) {
		return NULL;
	}

	while (index-- > 0) {
		at = items[at].next;
	}

	return &items[at];
}

/* --- painting ------------------------------------------------------------------ */

static void paint_row(lv_obj_t *row, bool selected)
{
	lv_obj_t *label = lv_obj_get_child(row, 0);

	lv_obj_set_style_bg_color(row,
				  lv_color_hex(selected ? ZD_C_SELECT : ZD_C_FACE),
				  LV_PART_MAIN);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);

	if (label != NULL) {
		lv_obj_set_style_text_color(
			label, lv_color_hex(selected ? ZD_C_SELECT_TEXT : ZD_C_TEXT),
			LV_PART_MAIN);
	}
}

/**
 * Scroll a row into view.
 *
 * By hand rather than through lv_obj_scroll_to_view(), which animates and reads
 * the object's laid-out coordinates -- neither of which is available in the
 * same breath as creating it. The rows are at known multiples of ROW_H, so the
 * arithmetic is exact and needs no layout pass.
 */
static void scroll_to(struct zd_rowlist *rl, int32_t index)
{
	int32_t top = lv_obj_get_scroll_y(rl->view);
	int32_t height = lv_obj_get_height(rl->view);
	int32_t want = index * ROW_H;

	if (height <= 0) {
		return; /* not laid out yet; the rebuild will place it */
	}

	if (want < top) {
		lv_obj_scroll_to_y(rl->view, want, LV_ANIM_OFF);
	} else if (want + ROW_H > top + height) {
		lv_obj_scroll_to_y(rl->view, want + ROW_H - height, LV_ANIM_OFF);
	}
}

/* --- row events ---------------------------------------------------------------- */

/*
 * Is this callback still talking about the list the user clicked?
 *
 * See the header comment: a CLICKED can arrive at a row whose model was
 * replaced by the DOUBLE_CLICKED that preceded it, microseconds earlier and on
 * the same release. Everything a row callback does starts here.
 */
static struct zd_rowlist *live_row(lv_event_t *e, int32_t *index)
{
	struct zd_rowlist *rl = lv_event_get_user_data(e);
	lv_obj_t *row = lv_event_get_target(e);

	if (rl == NULL || !rl->used || rl->dirty) {
		return NULL;
	}

	*index = lv_obj_get_index(row);
	if (*index < 0 || *index >= rl->count) {
		return NULL;
	}

	return rl;
}

static void select_index(struct zd_rowlist *rl, int32_t index, bool notify)
{
	int32_t was = rl->selected;
	lv_obj_t *row;

	if (was == index) {
		return;
	}

	rl->selected = (int16_t)index;

	/* Repaint the two rows that changed rather than rebuilding: a rebuild
	 * would lose the scroll position, and arrowing down a long list would
	 * jump back to the top on every key.
	 */
	if (!rl->dirty) {
		if (was >= 0 && was < rl->count) {
			row = lv_obj_get_child(rl->view, was);
			if (row != NULL) {
				paint_row(row, false);
			}
		}
		if (index >= 0 && index < rl->count) {
			row = lv_obj_get_child(rl->view, index);
			if (row != NULL) {
				paint_row(row, true);
			}
			scroll_to(rl, index);
		}
	}

	if (notify && rl->on_select != NULL) {
		struct row_item *item = item_at(rl, index);

		rl->on_select(rl->user, index, item != NULL ? item->id : 0);
	}
}

static void row_clicked(lv_event_t *e)
{
	int32_t index;
	struct zd_rowlist *rl = live_row(e, &index);

	if (rl == NULL) {
		return;
	}

	select_index(rl, index, true);

	/* The press already bubbled up and raised the window, which is what
	 * bubbling is for here. Stop the click, so a zapp is not told both
	 * "row 3 selected" and "you were clicked at (40,72)" for one gesture.
	 */
	lv_event_stop_bubbling(e);
}

static void row_activated(lv_event_t *e)
{
	int32_t index;
	struct zd_rowlist *rl = live_row(e, &index);
	struct row_item *item;

	if (rl == NULL) {
		return;
	}

	select_index(rl, index, false);

	item = item_at(rl, index);
	if (rl->on_activate != NULL) {
		rl->on_activate(rl->user, index, item != NULL ? item->id : 0);
	}

	lv_event_stop_bubbling(e);
}

/*
 * Released by LVGL, not by us -- the same arrangement text_api.c uses, and for
 * the same reason. A list dies either because its owner destroyed it or because
 * the window it lives in was reaped and lv_obj_delete() took the whole subtree.
 * Hanging teardown on LV_EVENT_DELETE covers both with one path, so there is no
 * way to close a window and leak a list or the rows it was holding.
 */
static void view_deleted(lv_event_t *e)
{
	struct zd_rowlist *rl = lv_event_get_user_data(e);

	if (rl == NULL || !rl->used) {
		return;
	}

	chain_release(rl);
	rl->view = NULL;
	rl->used = false;
	rl->dirty = false;
	live_lists--;
}

/* --- creation ------------------------------------------------------------------ */

int16_t zd_rowlist_row_h(void)
{
	return ROW_H;
}

int zd_rowlist_capacity(void)
{
	return CONFIG_ZD_LIST_MAX_ITEMS;
}

struct zd_rowlist *zd_rowlist_create(lv_obj_t *parent, int16_t x, int16_t y, int16_t w,
				     int16_t h)
{
	struct zd_rowlist *rl = NULL;
	lv_obj_t *view;

	for (size_t i = 0; i < ARRAY_SIZE(lists); i++) {
		if (!lists[i].used) {
			rl = &lists[i];
			break;
		}
	}

	if (rl == NULL) {
		LOG_WRN("list table full (%d)", CONFIG_ZD_MAX_LISTS);
		return NULL;
	}

	view = lv_obj_create(parent);
	lv_obj_remove_style_all(view);
	lv_obj_add_style(view, &zd_style_face, LV_PART_MAIN);
	lv_obj_set_style_bg_color(view, lv_color_hex(ZD_C_LIGHT), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(view, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_pos(view, x, y);
	lv_obj_set_size(view, w, h);

	/* zd_style_face turns scrolling off, so it goes back on explicitly --
	 * vertically only, because a row is exactly as wide as the list.
	 */
	lv_obj_add_flag(view, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(view, LV_DIR_VER);
	/* On by default, and wrong here: a list already at its limit would hand
	 * the rest of the drag to whatever is underneath, which in a window is
	 * the content area and in a dialog is the panel.
	 */
	lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLL_CHAIN);
	/* So a press inside still raises and focuses the window, exactly as it
	 * does for a label or a text widget. The click is stopped per row.
	 */
	lv_obj_add_flag(view, LV_OBJ_FLAG_EVENT_BUBBLE);

	zd_bevel_attach(view, ZD_BEVEL_IN);

	rl->view = view;
	rl->head = NO_ITEM;
	rl->tail = NO_ITEM;
	rl->count = 0;
	rl->selected = -1;
	rl->dirty = false;
	rl->used = true;
	rl->on_select = NULL;
	rl->on_activate = NULL;
	rl->user = NULL;

	live_lists++;

	lv_obj_add_event_cb(view, view_deleted, LV_EVENT_DELETE, rl);

	return rl;
}

void zd_rowlist_destroy(struct zd_rowlist *rl)
{
	if (rl != NULL && rl->used) {
		/* The DELETE handler does the bookkeeping, so this takes the
		 * same path a window close takes.
		 */
		lv_obj_delete(rl->view);
	}
}

void zd_rowlist_set_cb(struct zd_rowlist *rl, zd_rowlist_cb_t on_select,
		       zd_rowlist_cb_t on_activate, void *user)
{
	if (rl == NULL) {
		return;
	}

	rl->on_select = on_select;
	rl->on_activate = on_activate;
	rl->user = user;
}

void zd_rowlist_set_geometry(struct zd_rowlist *rl, int16_t x, int16_t y, int16_t w,
			     int16_t h)
{
	if (rl == NULL || !rl->used) {
		return;
	}

	lv_obj_set_pos(rl->view, x, y);
	lv_obj_set_size(rl->view, w, h);
}

lv_obj_t *zd_rowlist_obj(const struct zd_rowlist *rl)
{
	return (rl != NULL && rl->used) ? rl->view : NULL;
}

/* --- the model ----------------------------------------------------------------- */

void zd_rowlist_clear(struct zd_rowlist *rl)
{
	if (rl == NULL || !rl->used) {
		return;
	}

	chain_release(rl);
	rl->selected = -1;
	rl->dirty = true;

	/* Hidden now, emptied later. A hidden object is out of the hit test, so
	 * no new press can reach a row describing a directory we have left.
	 * The same "hide, then reap" idiom as zd_menu_close().
	 */
	lv_obj_add_flag(rl->view, LV_OBJ_FLAG_HIDDEN);
}

int zd_rowlist_add(struct zd_rowlist *rl, const char *text, uint16_t id)
{
	int16_t index;

	if (rl == NULL || !rl->used || text == NULL) {
		return -EINVAL;
	}

	if (rl->count >= CONFIG_ZD_LIST_MAX_ITEMS) {
		return -ENOSPC;
	}

	index = item_alloc();
	if (index == NO_ITEM) {
		/* The shared pool is out, which is a different failure from
		 * this list being full and deserves to be visible: it means
		 * some other list is holding everything.
		 */
		LOG_WRN("list row pool exhausted (%d)", CONFIG_ZD_LIST_ITEMS_TOTAL);
		return -ENOSPC;
	}

	(void)strncpy(items[index].text, text, sizeof(items[index].text) - 1);
	items[index].text[sizeof(items[index].text) - 1] = '\0';
	items[index].id = id;
	items[index].next = NO_ITEM;

	if (rl->tail == NO_ITEM) {
		rl->head = index;
	} else {
		items[rl->tail].next = index;
	}
	rl->tail = index;

	rl->dirty = true;

	return rl->count++;
}

int zd_rowlist_count(const struct zd_rowlist *rl)
{
	return (rl != NULL && rl->used) ? rl->count : -EINVAL;
}

int zd_rowlist_selected(const struct zd_rowlist *rl)
{
	return (rl != NULL && rl->used) ? rl->selected : -EINVAL;
}

int zd_rowlist_select(struct zd_rowlist *rl, int32_t index)
{
	if (rl == NULL || !rl->used) {
		return -EINVAL;
	}

	if (index < -1 || index >= rl->count) {
		return -EINVAL;
	}

	select_index(rl, index, false);
	return 0;
}

int zd_rowlist_item_id(const struct zd_rowlist *rl, int32_t index)
{
	struct row_item *item;

	if (rl == NULL || !rl->used) {
		return -EINVAL;
	}

	item = item_at(rl, index);
	return item != NULL ? (int)item->id : -ENOENT;
}

int zd_rowlist_item_text(const struct zd_rowlist *rl, int32_t index, char *buf,
			 uint32_t len)
{
	struct row_item *item;
	size_t need;

	if (rl == NULL || !rl->used || buf == NULL || len == 0) {
		return -EINVAL;
	}

	item = item_at(rl, index);
	if (item == NULL) {
		return -ENOENT;
	}

	need = strlen(item->text);
	if (need + 1 > len) {
		/* Refuse rather than truncate. Half a filename still names a
		 * file, just the wrong one -- the same call set_path() makes.
		 */
		return -ENOSPC;
	}

	memcpy(buf, item->text, need + 1);
	return (int)need;
}

/* --- keys ---------------------------------------------------------------------- */

bool zd_rowlist_key(struct zd_rowlist *rl, uint32_t code, uint16_t mods)
{
	int32_t at;
	int32_t page;

	ARG_UNUSED(mods);

	if (rl == NULL || !rl->used || rl->count == 0) {
		return false;
	}

	at = rl->selected;
	page = MAX(1, lv_obj_get_height(rl->view) / ROW_H);

	switch (code) {
	case ZD_KEY_UP:
		at = at <= 0 ? 0 : at - 1;
		break;

	case ZD_KEY_DOWN:
		/* From nothing selected, Down lands on the first row rather
		 * than the second, which is what every list box does.
		 */
		at = at < 0 ? 0 : MIN(at + 1, rl->count - 1);
		break;

	case ZD_KEY_PAGE_UP:
		at = at <= 0 ? 0 : MAX(0, at - page);
		break;

	case ZD_KEY_PAGE_DOWN:
		at = at < 0 ? 0 : MIN(at + page, rl->count - 1);
		break;

	case ZD_KEY_HOME:
		at = 0;
		break;

	case ZD_KEY_END:
		at = rl->count - 1;
		break;

	case ZD_KEY_ENTER:
		if (rl->selected < 0) {
			return false; /* nothing to open */
		}
		if (rl->on_activate != NULL) {
			struct row_item *item = item_at(rl, rl->selected);

			rl->on_activate(rl->user, rl->selected,
					item != NULL ? item->id : 0);
		}
		return true;

	default:
		/* Letters, Escape and the function keys mean nothing to a list
		 * and everything to whoever owns it.
		 */
		return false;
	}

	select_index(rl, at, true);
	return true;
}

/* --- the reap ------------------------------------------------------------------ */

static void rebuild(struct zd_rowlist *rl)
{
	int16_t index = rl->head;
	int32_t w = lv_obj_get_width(rl->view);
	int32_t y = 0;
	int32_t n = 0;

	lv_obj_clean(rl->view);

	/* Before the first layout pass the container answers 0, and rows built
	 * at width 0 stay that way. Force one; the same trick chrome/menu.c
	 * uses to measure a label.
	 */
	if (w <= 0) {
		lv_obj_update_layout(rl->view);
		w = lv_obj_get_width(rl->view);
	}

	while (index != NO_ITEM) {
		lv_obj_t *row = lv_obj_create(rl->view);
		lv_obj_t *label;

		lv_obj_remove_style_all(row);
		lv_obj_add_style(row, &zd_style_face, LV_PART_MAIN);
		lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_size(row, w, ROW_H);
		lv_obj_set_pos(row, 0, y);

		label = lv_label_create(row);
		lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
		lv_obj_set_width(label, w - 2 * ROW_PAD_X);
		lv_obj_set_pos(label, ROW_PAD_X, LABEL_Y);
		lv_label_set_text(label, items[index].text);

		paint_row(row, n == rl->selected);

		lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, rl);
		lv_obj_add_event_cb(row, row_activated, LV_EVENT_DOUBLE_CLICKED, rl);

		y += ROW_H;
		n++;
		index = items[index].next;
	}

	lv_obj_remove_flag(rl->view, LV_OBJ_FLAG_HIDDEN);
	rl->dirty = false;

	if (rl->selected >= 0) {
		scroll_to(rl, rl->selected);
	}
}

void zd_rowlist_reap(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(lists); i++) {
		if (lists[i].used && lists[i].dirty) {
			rebuild(&lists[i]);
		}
	}
}

uint32_t zd_rowlist_live_count(void)
{
	return live_lists;
}

uint32_t zd_rowlist_items_used(void)
{
	return items_used;
}
