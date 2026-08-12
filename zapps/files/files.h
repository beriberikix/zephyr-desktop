/*
 * files — shared state and command ids for the file browser.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZAPPS_FILES_FILES_H_
#define ZAPPS_FILES_FILES_H_

#include <stdbool.h>

#include <lib/zapplib.h>
#include <zd/zapp_abi.h>

/*
 * Most rows this zapp will keep details for.
 *
 * Its own number, not the desktop's. list_get_capacity() says how many rows the
 * list will hold and that may be larger or smaller than this; what this bounds
 * is the size and type we cached while filling it. A directory with more
 * entries than either bound is listed as far as it goes and says so.
 */
#define FB_MAX_ENTRIES 64

/* Menu command ids. Ours; the desktop only hands them back. */
#define FB_FILE_OPEN   1
#define FB_FILE_NEWDIR 2
#define FB_FILE_DELETE 3
#define FB_FILE_UP     4
#define FB_FILE_EXIT   5

#define FB_GO_HOME 10
#define FB_GO_TMP  11
#define FB_GO_APPS 12

/* Dialog ids. */
#define FB_DLG_NEWDIR 1
#define FB_DLG_DELETE 2

/*
 * Row ids.
 *
 * The type rides in the top bit, exactly as it does in the desktop's own file
 * picker, and for the same reason: the id is documented as ours, and a parallel
 * array of types indexed by row is the same fact kept somewhere it can drift
 * out of step with the list.
 */
#define FB_ROW_IS_DIR 0x8000u
#define FB_ROW_UP     0x7FFFu

struct fb_state {
	bool used;
	zd_window_t win;
	zd_list_t list;
	zd_label_t status;
	zd_menu_t file_menu;
	zd_menu_t go_menu;

	char root[ZD_PATH_MAX]; /**< the place we were sent to; the floor for "up" */
	char dir[ZD_PATH_MAX];  /**< where we are now */

	/* Cached while the directory was read, so that reacting to a selection
	 * costs nothing. ZD_EV_LIST_SELECT fires on every arrow key and the ABI
	 * says plainly not to do filesystem I/O in it; fs_readdir already gave
	 * us both of these, so keeping them is free.
	 */
	uint32_t size[FB_MAX_ENTRIES];
	uint8_t type[FB_MAX_ENTRIES];
	int count;
	bool truncated;

	int16_t content_w;
	int16_t content_h;

	bool relist; /**< the directory changed; refill from the event, not now */
};

extern const struct zd_host_api *fb_host;

/* --- browse.c ------------------------------------------------------------------ */

/** Read st->dir into the list. */
void fb_relist(zd_zapp_ctx_t ctx, struct fb_state *st);

/** Describe the selection, or the directory when there is none. */
void fb_status(zd_zapp_ctx_t ctx, struct fb_state *st);

/** Enter @p index. @return true if the directory changed. */
bool fb_activate(zd_zapp_ctx_t ctx, struct fb_state *st, int32_t index, uint16_t id);

/** Strip the last component of st->dir, floored at st->root. */
bool fb_up(struct fb_state *st);

/** Point st->dir (and its floor) at a well-known directory. */
bool fb_go(zd_zapp_ctx_t ctx, struct fb_state *st, enum zd_dir dir);

/** Point st->dir at the directory holding @p path, and select @p path's name. */
bool fb_reveal(struct fb_state *st, const char *path);

/* --- commands.c ----------------------------------------------------------------- */

void fb_command(zd_zapp_ctx_t ctx, struct fb_state *st, uint16_t id);
void fb_dialog_answered(zd_zapp_ctx_t ctx, struct fb_state *st, uint16_t id,
			int16_t result);

#endif /* ZAPPS_FILES_FILES_H_ */
