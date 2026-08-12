/*
 * files — the file browser, and the second zapp anybody would actually use.
 *
 * Notepad forced most of ABI 0.5 into existence. This one needed almost
 * nothing new from storage: fs_opendir, fs_readdir, fs_stat, fs_mkdir and
 * fs_unlink have all been in the ABI since 0.3 and no zapp had ever called one
 * of them. What was missing was a way to SHOW a directory -- a zapp never sees
 * an lv_obj_t, so before 0.6's list widget there was no way to put a scrolling
 * column of anything on screen.
 *
 * A SINGLETON, and deliberately, for three reasons that happen to agree. It
 * navigates in place, so a second window would be a second answer to "where am
 * I". It is the thing that launches other zapps, so it is the natural test of
 * the flag L5 made real -- an unexercised code path is what this project's
 * selftests exist to prevent. And llext loads one copy of an image, so a
 * singleton may keep state in file-scope .bss without the sharing hazard that
 * Notepad has to work around with a slot table.
 *
 * What is NOT here, and is not an oversight:
 *
 *   - Rename. fs_rename exists and it is three lines given the prompt dialog,
 *     but the useful version renames what you can see without first selecting
 *     it, and in-place editing of a list row is its own piece of work.
 *   - Sorting other than folders-then-name-as-found, and any column but the
 *     one. There is one proportional font and no table widget.
 *   - "Modified". struct zd_dirent has no timestamp, Zephyr's struct fs_dirent
 *     has none to give, and clock_now() has no date in it at all. The same
 *     three facts that took Time/Date back out of Notepad.
 *   - Noticing that somebody else changed the directory. Nothing notifies
 *     anyone of anything yet; reopening the folder is the refresh.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/llext/symbol.h>

#include "files.h"

const struct zd_host_api *fb_host;

/* One instance, guaranteed by ZD_ZAPP_FLAG_SINGLETON. See the header comment:
 * file scope is safe here precisely because of that flag, and would not be
 * without it.
 */
static struct fb_state state;

#define STATUS_H 14

static void layout(zd_zapp_ctx_t ctx, struct fb_state *st, int16_t w, int16_t h)
{
	struct zd_rect geom = { .x = 0, .y = 0, .w = w, .h = (int16_t)(h - STATUS_H) };

	st->content_w = w;
	st->content_h = h;

	fb_host->list_set_geometry(ctx, st->list, &geom);
	/* The status line follows the bottom edge, which is the whole reason
	 * 0.6 added label_set_pos(): a 0.5 label could be created and then
	 * never moved, and this window resizes.
	 */
	fb_host->label_set_pos(ctx, st->status, 2, (int16_t)(h - STATUS_H + 2));
}

static void build_menus(zd_zapp_ctx_t ctx, struct fb_state *st)
{
	zd_menu_t bar = fb_host->menubar_create(ctx, st->win);

	if (bar == NULL) {
		return; /* survivable: double-click still works */
	}

	st->file_menu = fb_host->menu_add_submenu(ctx, bar, "File");
	fb_host->menu_add_item(ctx, st->file_menu, "Open", FB_FILE_OPEN);
	fb_host->menu_add_item(ctx, st->file_menu, "Up", FB_FILE_UP);
	fb_host->menu_add_separator(ctx, st->file_menu);
	fb_host->menu_add_item(ctx, st->file_menu, "New Folder...", FB_FILE_NEWDIR);
	fb_host->menu_add_item(ctx, st->file_menu, "Delete", FB_FILE_DELETE);
	fb_host->menu_add_separator(ctx, st->file_menu);
	fb_host->menu_add_item(ctx, st->file_menu, "Exit", FB_FILE_EXIT);

	/* The nearest thing this desktop has to "My Computer": the well-known
	 * directories, named by hand. There is no ABI call that enumerates them
	 * and there does not need to be -- they are ABI constants, so a zapp
	 * built against 0.6 knows exactly which ones 0.6 has.
	 */
	st->go_menu = fb_host->menu_add_submenu(ctx, bar, "Go");
	fb_host->menu_add_item(ctx, st->go_menu, "Home", FB_GO_HOME);
	fb_host->menu_add_item(ctx, st->go_menu, "Temporary", FB_GO_TMP);
	fb_host->menu_add_item(ctx, st->go_menu, "Programs", FB_GO_APPS);
}

/** Title from the current directory, which is the only thing worth saying. */
static void retitle(zd_zapp_ctx_t ctx, struct fb_state *st)
{
	char title[ZD_TITLE_MAX];
	const char *leaf = z_basename(st->dir);
	uint32_t at;

	at = z_append(title, 0, sizeof(title), leaf[0] != '\0' ? leaf : "Files");
	at = at != 0 ? z_append(title, at, sizeof(title), " - Files") : 0;

	fb_host->window_set_title(ctx, st->win, at != 0 ? title : "Files");
}

static void refresh(zd_zapp_ctx_t ctx, struct fb_state *st)
{
	char msg[ZD_PATH_MAX + 16];
	uint32_t at;

	fb_relist(ctx, st);
	retitle(ctx, st);

	/* Permanent tracing, for the reason the picker's is: which directory is
	 * on screen is the one piece of this zapp's state that a screenshot
	 * cannot tell you, and two similar folders look identical.
	 */
	at = z_append(msg, 0, sizeof(msg), "showing ");
	at = at != 0 ? z_append(msg, at, sizeof(msg), st->dir) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), " (") : 0;
	at = at != 0 ? z_append_u32(msg, at, sizeof(msg), (uint32_t)st->count) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), " rows)") : 0;

	if (at != 0) {
		fb_host->log(ctx, 0, msg);
	}
}

static int files_init(zd_zapp_ctx_t ctx, const struct zd_host_api *api)
{
	struct zd_window_desc desc = {
		.title = "Files",
		.geom = { 0, 0, 0, 0 },
	};
	struct fb_state *st = &state;
	struct zd_rect geom = { 0, 0, 0, 0 };
	char arg[ZD_PATH_MAX];
	int16_t w;
	int16_t h;

	fb_host = api;

	z_zero(st, sizeof(*st));
	st->used = true;

	api->set_user_data(ctx, st);

	st->win = api->window_create(ctx, &desc);
	if (st->win == NULL) {
		return -1;
	}

	/* The bar first, then everything sized to what is left -- events are
	 * suppressed during init(), so a correction from ZD_EV_RESIZED would
	 * not arrive until the first real resize.
	 */
	build_menus(ctx, st);

	if (api->window_get_content_size(ctx, st->win, &w, &h) != 0) {
		return -1;
	}

	geom.w = w;
	geom.h = (int16_t)(h - STATUS_H);
	st->list = api->list_create(ctx, st->win, &geom, 0);
	if (st->list == NULL) {
		return -1;
	}

	st->status = api->label_create(ctx, st->win, "", 2, (int16_t)(h - STATUS_H + 2));
	st->content_w = w;
	st->content_h = h;

	/* Wherever we were pointed, or home. A path we were launched with names
	 * a file, so show the folder holding it -- "reveal this" is what being
	 * handed a path means to a browser.
	 */
	if (api->get_launch_arg(ctx, arg, sizeof(arg)) <= 0 || !fb_reveal(st, arg)) {
		(void)fb_go(ctx, st, ZD_DIR_HOME);
	}

	refresh(ctx, st);

	api->log(ctx, 0, "Files ready");
	return 0;
}

static void files_event(zd_zapp_ctx_t ctx, const struct zd_event *ev)
{
	struct fb_state *st = fb_host->get_user_data(ctx);
	char arg[ZD_PATH_MAX];

	if (st == NULL || (ev->type != ZD_EV_LAUNCH_ARG && ev->win != st->win)) {
		return;
	}

	switch (ev->type) {
	case ZD_EV_RESIZED:
		layout(ctx, st, ev->resize.w, ev->resize.h);
		break;

	case ZD_EV_LIST_SELECT:
		/* Cheap by contract, and it is: everything shown here was read
		 * once, while the directory was being listed.
		 */
		fb_status(ctx, st);
		break;

	case ZD_EV_LIST_ACTIVATE:
		/*
		 * Entering a folder means emptying and refilling the very list
		 * whose row is dispatching this. That is safe, and it is the
		 * reason the list widget keeps a model: nothing on screen is
		 * touched until the desktop's loop comes round.
		 *
		 * The relist still waits until after fb_activate() returns
		 * rather than happening inside it, because the row text is read
		 * out of the list and clearing it first would throw away the
		 * name we are navigating to.
		 */
		st->relist = fb_activate(ctx, st, ev->list.index, ev->list.id);
		break;

	case ZD_EV_MENU:
		fb_command(ctx, st, ev->menu.id);
		break;

	case ZD_EV_DIALOG:
		fb_dialog_answered(ctx, st, ev->dialog.id, ev->dialog.result);
		break;

	case ZD_EV_LAUNCH_ARG:
		/* Somebody launched us again. Being a singleton, this is what
		 * arrives instead of a second window.
		 */
		if (fb_host->get_launch_arg(ctx, arg, sizeof(arg)) > 0 &&
		    fb_reveal(st, arg)) {
			st->relist = true;
		}
		break;

	case ZD_EV_WINDOW_CLOSE_REQUEST:
		/* Nothing unsaved, so there is nothing to ask about. A browser
		 * that argued on the way out would be a browser nobody closes.
		 */
		st->list = NULL;
		fb_host->window_close(ctx, st->win);
		return;

	default:
		break;
	}

	/* One place, at the end, after every path that could have moved us. Not
	 * inside the handlers: a menu command and an activation and a finished
	 * dialog can each change the directory, and three copies of this is
	 * three chances to forget one.
	 */
	if (st->relist) {
		st->relist = false;
		refresh(ctx, st);
	}
}

static void files_fini(zd_zapp_ctx_t ctx)
{
	struct fb_state *st = fb_host->get_user_data(ctx);

	if (st != NULL) {
		st->used = false;
	}

	fb_host->log(ctx, 0, "Files closed");
}

struct zd_zapp_manifest zd_zapp_manifest = {
	.magic = ZD_ZAPP_MAGIC,
	.abi_major = ZD_ABI_MAJOR,
	.abi_minor = ZD_ABI_MINOR,
	.flags = ZD_ZAPP_FLAG_SINGLETON,
	.name = "Files",
	.icon = NULL,
	.init = files_init,
	.event = files_event,
	.fini = files_fini,
};
LL_EXTENSION_SYMBOL(zd_zapp_manifest);
