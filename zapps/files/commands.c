/*
 * files — what the menu items do, and what the dialogs answer.
 *
 * The two write operations here are the first time a zapp has asked the
 * permission shim for anything it might not get. Notepad only ever wrote where
 * a picker had already sent it; New Folder and Delete can be aimed anywhere the
 * user has navigated to, including the read-only system directory. Both simply
 * report what came back -- the shim is the authority, and second-guessing it
 * here by greying out menu items would mean this zapp keeping its own model of
 * a permission set it cannot see.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "files.h"

/*
 * Say something went wrong, since there is nowhere else to say it.
 *
 * @p err is a value the DESKTOP returned, so naming one of them is reporting
 * what was said rather than inventing an explanation. That is the difference
 * from Notepad, which defines its own NP_E_* space precisely because its
 * failures are its own and borrowing errno for them would misattribute them.
 * Only -EACCES is picked out, because it is the one the user can act on: it
 * means "not here, try somewhere you own".
 */
#define FB_EACCES 13 /* the desktop's -EACCES, positive; see fs_shim.c */

static void complain(zd_zapp_ctx_t ctx, const char *what, int err)
{
	char msg[96];
	uint32_t at;

	at = z_append(msg, 0, sizeof(msg), what);

	if (err == -FB_EACCES) {
		at = at != 0 ? z_append(msg, at, sizeof(msg),
					"\n\nThis folder is read-only.")
			     : 0;
	} else if (err != 0) {
		at = at != 0 ? z_append(msg, at, sizeof(msg), "\n\nError ") : 0;
		at = at != 0 ? z_append_u32(msg, at, sizeof(msg), (uint32_t)(-err)) : 0;
		at = at != 0 ? z_append(msg, at, sizeof(msg), ".") : 0;
	}

	if (at != 0) {
		(void)fb_host->dialog_confirm(ctx, "Files", msg, ZD_DLG_OK_ONLY, 0);
		fb_host->log(ctx, 0, msg);
	}
}

/** The full path of the selected row, or false if nothing usable is chosen. */
static bool selected_path(zd_zapp_ctx_t ctx, struct fb_state *st, char *out, uint32_t cap,
			  bool *is_dir)
{
	char name[ZD_NAME_MAX];
	int selected = fb_host->list_get_selected(ctx, st->list);
	int id;
	uint32_t len;

	if (selected < 0) {
		return false;
	}

	id = fb_host->list_get_item_id(ctx, st->list, selected);
	if (id < 0 || (uint16_t)id == FB_ROW_UP) {
		return false; /* ".." is a label, not a thing to act on */
	}

	if (fb_host->list_get_item_text(ctx, st->list, selected, name, sizeof(name)) < 0) {
		return false;
	}

	*is_dir = ((uint16_t)id & FB_ROW_IS_DIR) != 0;

	len = z_len(name);
	if (*is_dir && len > 0 && name[len - 1] == '/') {
		name[len - 1] = '\0';
	}

	return z_path_join(out, cap, st->dir, name);
}

void fb_command(zd_zapp_ctx_t ctx, struct fb_state *st, uint16_t id)
{
	char path[ZD_PATH_MAX];
	char msg[128];
	bool is_dir = false;
	uint32_t at;

	switch (id) {
	case FB_FILE_OPEN:
		/* The same thing a double-click does, for anyone who would
		 * rather use the menu -- and the only route to it from a
		 * keyboard-driven session that never touches the panel.
		 */
		{
			int selected = fb_host->list_get_selected(ctx, st->list);
			int row_id;

			if (selected < 0) {
				break;
			}
			row_id = fb_host->list_get_item_id(ctx, st->list, selected);
			if (row_id < 0) {
				break;
			}
			st->relist = fb_activate(ctx, st, selected, (uint16_t)row_id);
		}
		break;

	case FB_FILE_UP:
		st->relist = fb_up(st);
		break;

	case FB_FILE_NEWDIR:
		(void)fb_host->dialog_prompt(ctx, "New Folder", "Name the new folder:",
					     "", FB_DLG_NEWDIR);
		break;

	case FB_FILE_DELETE:
		if (!selected_path(ctx, st, path, sizeof(path), &is_dir)) {
			break;
		}

		at = z_append(msg, 0, sizeof(msg), "Delete '");
		at = at != 0 ? z_append(msg, at, sizeof(msg), z_basename(path)) : 0;
		at = at != 0 ? z_append(msg, at, sizeof(msg), "'?") : 0;
		if (at != 0 && is_dir) {
			/* Say it, because the desktop cannot: fs_unlink on a
			 * directory with anything in it fails, and the user
			 * deserves to know that before they click Yes rather
			 * than after.
			 */
			at = z_append(msg, at, sizeof(msg),
				      "\n\nA folder must be empty first.");
		}

		if (at != 0) {
			(void)fb_host->dialog_confirm(ctx, "Files", msg,
						      ZD_DLG_OK_CANCEL, FB_DLG_DELETE);
		}
		break;

	case FB_GO_HOME:
		st->relist = fb_go(ctx, st, ZD_DIR_HOME);
		break;

	case FB_GO_TMP:
		st->relist = fb_go(ctx, st, ZD_DIR_TMP);
		break;

	case FB_GO_APPS:
		st->relist = fb_go(ctx, st, ZD_DIR_SYSTEM_ZAPPS);
		break;

	case FB_FILE_EXIT:
		/* Nothing unsaved to argue about, so this is the whole of it.
		 * The window goes away in the desktop's reap after this returns,
		 * which is why it is the last statement.
		 */
		st->list = NULL;
		fb_host->window_close(ctx, st->win);
		break;

	default:
		break;
	}
}

void fb_dialog_answered(zd_zapp_ctx_t ctx, struct fb_state *st, uint16_t id,
			int16_t result)
{
	char name[ZD_NAME_MAX];
	char path[ZD_PATH_MAX];
	bool is_dir = false;
	int ret;

	switch (id) {
	case FB_DLG_NEWDIR:
		if (result != ZD_DLG_OK) {
			break;
		}
		if (fb_host->dialog_get_text(ctx, name, sizeof(name)) <= 0) {
			break;
		}
		if (!z_path_join(path, sizeof(path), st->dir, name)) {
			complain(ctx, "That name is too long for this folder.", 0);
			break;
		}

		ret = fb_host->fs_mkdir(ctx, path);
		if (ret != 0) {
			complain(ctx, "Cannot make that folder.", ret);
			break;
		}

		st->relist = true;
		break;

	case FB_DLG_DELETE:
		if (result != ZD_DLG_OK) {
			break;
		}
		if (!selected_path(ctx, st, path, sizeof(path), &is_dir)) {
			break;
		}

		ret = fb_host->fs_unlink(ctx, path);
		if (ret != 0) {
			complain(ctx, "Cannot delete that.", ret);
			break;
		}

		st->relist = true;
		break;

	default:
		break;
	}
}
