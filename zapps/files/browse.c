/*
 * files — reading a directory and saying what is in it.
 *
 * The first zapp to call fs_opendir()/fs_readdir(). Those slots have been in
 * the ABI since 0.3 and nothing has ever used them: storage arrived for
 * Notepad, which only ever needed to open a path somebody else had chosen.
 *
 * Two things here are worth knowing.
 *
 * ONE. The listing is read once and the size and type of every entry are kept.
 * ZD_EV_LIST_SELECT fires on every arrow key, and the ABI says in as many words
 * not to do filesystem I/O in it -- on a board where storage borrows the
 * display's pin, a stat per keystroke would stop the screen drawing while the
 * user held Down. fs_readdir handed us both fields while we were filling the
 * list, so keeping them costs 320 bytes and removes the temptation entirely.
 *
 * TWO. Going up is a string truncation, not "..". The desktop's shim rejects
 * that as a path component on purpose, so that the shim and the filesystem can
 * never disagree about what a path means. z_parent() is what a zapp uses
 * instead, and the ".." shown in the list is a label.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "files.h"

/** Append a size the way a Win95 status bar did: "1,204 bytes", near enough. */
static uint32_t append_size(char *dst, uint32_t at, uint32_t cap, uint32_t bytes)
{
	at = z_append_u32(dst, at, cap, bytes);
	return at != 0 ? z_append(dst, at, cap, bytes == 1 ? " byte" : " bytes") : 0;
}

void fb_status(zd_zapp_ctx_t ctx, struct fb_state *st)
{
	char msg[96];
	char name[ZD_NAME_MAX];
	int selected = fb_host->list_get_selected(ctx, st->list);
	uint32_t at = 0;

	if (selected >= 0 && selected < st->count &&
	    fb_host->list_get_item_text(ctx, st->list, selected, name, sizeof(name)) >= 0) {
		at = z_append(msg, 0, sizeof(msg), name);

		if (st->type[selected] == ZD_DIRENT_DIR) {
			at = at != 0 ? z_append(msg, at, sizeof(msg), "   folder") : 0;
		} else {
			at = at != 0 ? z_append(msg, at, sizeof(msg), "   ") : 0;
			at = at != 0 ? append_size(msg, at, sizeof(msg),
						   st->size[selected])
				     : 0;
		}
	} else {
		/* Nothing chosen: describe the directory instead. Win95's status
		 * bar did exactly this, and it is the only place the ".." row is
		 * excluded from the count -- it is not an object in here.
		 */
		int shown = st->count;

		if (!z_eq(st->dir, st->root) && shown > 0) {
			shown--;
		}

		at = z_append_u32(msg, 0, sizeof(msg), (uint32_t)shown);
		at = at != 0 ? z_append(msg, at, sizeof(msg),
					shown == 1 ? " object" : " objects")
			     : 0;
		if (st->truncated) {
			at = at != 0 ? z_append(msg, at, sizeof(msg), " (more not shown)")
				     : 0;
		}
	}

	if (at != 0) {
		fb_host->label_set_text(ctx, st->status, msg);
	}
}

void fb_relist(zd_zapp_ctx_t ctx, struct fb_state *st)
{
	struct zd_dirent entry;
	char label[ZD_NAME_MAX];
	zd_dir_t dir;
	int capacity;
	int pass;

	fb_host->list_clear(ctx, st->list);
	st->count = 0;
	st->truncated = false;

	capacity = fb_host->list_get_capacity(ctx, st->list);
	if (capacity > FB_MAX_ENTRIES) {
		capacity = FB_MAX_ENTRIES;
	}

	if (!z_eq(st->dir, st->root)) {
		/* A label, never a path. See the header comment. */
		if (fb_host->list_add_item(ctx, st->list, "..", FB_ROW_UP) >= 0) {
			st->type[st->count] = ZD_DIRENT_DIR;
			st->size[st->count] = 0;
			st->count++;
		}
	}

	/* Two passes so folders come first, which is how every file manager has
	 * ever done it and costs one extra opendir on a directory small enough
	 * to fit in a 64-row list.
	 */
	for (pass = 0; pass < 2; pass++) {
		bool want_dirs = pass == 0;

		if (fb_host->fs_opendir(ctx, st->dir, &dir) != 0) {
			return;
		}

		while (fb_host->fs_readdir(ctx, dir, &entry) == 0) {
			bool is_dir = entry.type == ZD_DIRENT_DIR;
			uint32_t at;

			if (is_dir != want_dirs) {
				continue;
			}

			if (st->count >= capacity) {
				st->truncated = true;
				break;
			}

			at = z_strcpy(label, sizeof(label), entry.name);
			if (is_dir) {
				/* A trailing slash marks a folder without an
				 * icon -- there is no image support here at all
				 * -- and no real filename can contain one, so
				 * taking it back off later is unambiguous.
				 */
				(void)z_append(label, at, sizeof(label), "/");
			}

			if (fb_host->list_add_item(ctx, st->list, label,
						   (uint16_t)(is_dir ? FB_ROW_IS_DIR : 0)) <
			    0) {
				st->truncated = true;
				break;
			}

			st->type[st->count] = entry.type;
			st->size[st->count] = entry.size;
			st->count++;
		}

		fb_host->fs_closedir(ctx, dir);
	}

	fb_status(ctx, st);
}

bool fb_up(struct fb_state *st)
{
	char probe[ZD_PATH_MAX];

	if (z_eq(st->dir, st->root)) {
		return false;
	}

	(void)z_strcpy(probe, sizeof(probe), st->dir);
	if (!z_parent(probe)) {
		return false;
	}

	(void)z_strcpy(st->dir, sizeof(st->dir), probe);
	return true;
}

bool fb_go(zd_zapp_ctx_t ctx, struct fb_state *st, enum zd_dir dir)
{
	char probe[ZD_PATH_MAX];

	if (fb_host->path_resolve(ctx, dir, probe, sizeof(probe)) != 0) {
		return false;
	}

	(void)z_strcpy(st->dir, sizeof(st->dir), probe);
	/* The floor moves with us. "Up" out of the place you asked for should
	 * not be possible, wherever that place was -- and the desktop would
	 * refuse it anyway, with an -EACCES nobody can act on.
	 */
	(void)z_strcpy(st->root, sizeof(st->root), probe);
	return true;
}

bool fb_reveal(struct fb_state *st, const char *path)
{
	char probe[ZD_PATH_MAX];

	if (path == NULL || path[0] != '/') {
		return false;
	}

	(void)z_strcpy(probe, sizeof(probe), path);
	if (!z_parent(probe)) {
		return false;
	}

	(void)z_strcpy(st->dir, sizeof(st->dir), probe);
	(void)z_strcpy(st->root, sizeof(st->root), probe);
	return true;
}

bool fb_activate(zd_zapp_ctx_t ctx, struct fb_state *st, int32_t index, uint16_t id)
{
	char name[ZD_NAME_MAX];
	char next[ZD_PATH_MAX];
	uint32_t len;

	if (id == FB_ROW_UP) {
		return fb_up(st);
	}

	if (fb_host->list_get_item_text(ctx, st->list, index, name, sizeof(name)) < 0) {
		return false;
	}

	if ((id & FB_ROW_IS_DIR) != 0) {
		len = z_len(name);
		if (len > 0 && name[len - 1] == '/') {
			name[len - 1] = '\0';
		}

		if (!z_path_join(next, sizeof(next), st->dir, name)) {
			fb_host->log(ctx, 0, "that folder is too deep to open");
			return false;
		}

		(void)z_strcpy(st->dir, sizeof(st->dir), next);
		return true;
	}

	/*
	 * A file. Hand it to Notepad, and say plainly that the mapping is
	 * hardcoded: there is no association registry in this desktop, and
	 * pretending otherwise by dressing this up as a lookup would be a lie
	 * about how finished it is. Notepad is the only zapp that opens
	 * anything, so it is the only answer there could be.
	 */
	if (!z_path_join(next, sizeof(next), st->dir, name)) {
		return false;
	}

	if (fb_host->zapp_launch(ctx, "notepad", next) != 0) {
		fb_host->log(ctx, 0, "cannot open that");
	}

	return false;
}
