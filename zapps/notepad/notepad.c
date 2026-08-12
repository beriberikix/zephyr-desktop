/*
 * notepad — the first zapp anybody would use, modelled on the original.
 *
 * hello, notes and badabi are instruments: they exist to prove a window can be
 * opened, a file written, a bad ABI refused. This one is an application. It
 * types, it selects, it cuts and pastes, it opens and saves, and it argues with
 * you on the way out if you have not saved.
 *
 * Everything it does, it does through the ABI. There is no libc here, no LVGL,
 * no Zephyr; it does not know what a filesystem or a font or a touch panel is.
 * The list of things milestone K had to build for this file to be possible is
 * most of milestone K: multi-file zapps, a key path, an on-screen keyboard, a
 * text widget, a clipboard, menus, dialogs and a clock.
 *
 * What is NOT here, and is not an oversight:
 *
 *   - Undo. LVGL's textarea has no undo stack and writing one that survives
 *     cut, paste and select-all is its own milestone, not a corner of this one.
 *   - Find, Replace, word-wrap toggle, print, page setup. Each needs a dialog
 *     or a service that does not exist yet; a greyed-out Search menu would be
 *     a lie about how finished this is.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/llext/symbol.h>

#include "notepad.h"

const struct zd_host_api *np_host;

/*
 * Per instance, not per image.
 *
 * llext refcounts by name: two Notepads share one copy of this file's .bss, so
 * a file-scope `static zd_text_t text` would have the second window quietly
 * editing the first one's document. Everything that differs between instances
 * lives in one of these and hangs off set_user_data().
 */
static struct np_state states[NP_MAX_INSTANCES];

static struct np_state *claim_state(void)
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

/** Make the text widget fill the content area, whatever size that now is. */
static void fit_text(zd_zapp_ctx_t ctx, struct np_state *st, int16_t w, int16_t h)
{
	struct zd_rect geom = { .x = 0, .y = 0, .w = w, .h = h };

	np_host->text_set_geometry(ctx, st->text, &geom);
}

static void build_menus(zd_zapp_ctx_t ctx, struct np_state *st)
{
	zd_menu_t bar = np_host->menubar_create(ctx, st->win);

	if (bar == NULL) {
		return; /* no menu bar is survivable; the accelerators still work */
	}

	st->file_menu = np_host->menu_add_submenu(ctx, bar, "File");
	np_host->menu_add_item(ctx, st->file_menu, "New", NP_FILE_NEW);
	np_host->menu_add_item(ctx, st->file_menu, "Open...", NP_FILE_OPEN);
	np_host->menu_add_item(ctx, st->file_menu, "Save", NP_FILE_SAVE);
	np_host->menu_add_item(ctx, st->file_menu, "Save As...", NP_FILE_SAVEAS);
	np_host->menu_add_separator(ctx, st->file_menu);
	np_host->menu_add_item(ctx, st->file_menu, "Exit", NP_FILE_EXIT);

	st->edit_menu = np_host->menu_add_submenu(ctx, bar, "Edit");
	np_host->menu_add_item(ctx, st->edit_menu, "Cut", NP_EDIT_CUT);
	np_host->menu_add_item(ctx, st->edit_menu, "Copy", NP_EDIT_COPY);
	np_host->menu_add_item(ctx, st->edit_menu, "Paste", NP_EDIT_PASTE);
	np_host->menu_add_item(ctx, st->edit_menu, "Delete", NP_EDIT_DELETE);
	np_host->menu_add_separator(ctx, st->edit_menu);
	np_host->menu_add_item(ctx, st->edit_menu, "Select All", NP_EDIT_SELALL);
	np_host->menu_add_item(ctx, st->edit_menu, "Time", NP_EDIT_TIME);
}

static int notepad_init(zd_zapp_ctx_t ctx, const struct zd_host_api *api)
{
	struct np_state *st;
	struct zd_window_desc desc = {
		.title = "Untitled - Notepad",
		.geom = { 0, 0, 0, 0 }, /* let the desktop place and cascade us */
	};
	struct zd_rect geom = { 0, 0, 0, 0 };
	int16_t w;
	int16_t h;

	np_host = api;

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

	/* The menu bar first, then the text widget sized to what is left. Doing
	 * it the other way round works too -- adding a bar shrinks the content
	 * area and delivers ZD_EV_RESIZED -- but events are suppressed during
	 * init(), so the correction would not arrive until the first real
	 * resize and the widget would overhang until then.
	 */
	build_menus(ctx, st);

	if (api->window_get_content_size(ctx, st->win, &w, &h) != 0) {
		st->used = false;
		return -1;
	}

	geom.w = w;
	geom.h = h;
	st->text = api->text_create(ctx, st->win, &geom, 0);
	if (st->text == NULL) {
		st->used = false;
		return -1;
	}

	api->log(ctx, 0, "Notepad ready");
	return 0;
}

static void notepad_event(zd_zapp_ctx_t ctx, const struct zd_event *ev)
{
	struct np_state *st = np_host->get_user_data(ctx);
	uint16_t accel;

	if (st == NULL || ev->win != st->win) {
		return;
	}

	switch (ev->type) {
	case ZD_EV_TEXT_CHANGED:
		/* The user typed. Edits this zapp made itself do not come back
		 * here, which is exactly what a dirty flag wants.
		 */
		if (!st->dirty) {
			st->dirty = true;
			np_doc_retitle(ctx, st);
		}
		break;

	case ZD_EV_RESIZED:
		fit_text(ctx, st, ev->resize.w, ev->resize.h);
		break;

	case ZD_EV_MENU:
		np_command(ctx, st, ev->menu.id);
		break;

	case ZD_EV_DIALOG:
		np_dialog_answered(ctx, st, ev->dialog.id, ev->dialog.result);
		break;

	case ZD_EV_KEY:
		/* Only accelerators reach here: ordinary typing is swallowed by
		 * the text widget, and the desktop routes anything held with
		 * CTRL to the zapp precisely so this can work while typing.
		 */
		accel = np_accelerator(ev->key.code, ev->key.unicode, ev->key.mods);
		if (accel != 0) {
			np_command(ctx, st, accel);
		} else if (ev->key.code == ZD_KEY_F(5)) {
			np_command(ctx, st, NP_EDIT_TIME); /* F5, as it always was */
		}
		break;

	case ZD_EV_WINDOW_CLOSE_REQUEST:
		/*
		 * The whole point of milestone J's handshake, finally used by
		 * something with an answer worth giving.
		 *
		 * If nothing is unsaved this closes at once. If something is,
		 * np_maybe_discard() puts up the confirm box and does NOT close
		 * -- and the desktop will not take the window while that box is
		 * on screen, because a zapp asking the user is not a zapp
		 * ignoring the request. Cancel leaves the window alone, and the
		 * next close request starts the conversation over.
		 */
		np_maybe_discard(ctx, st, NP_PENDING_EXIT);
		break;

	default:
		break;
	}
}

static void notepad_fini(zd_zapp_ctx_t ctx)
{
	struct np_state *st = np_host->get_user_data(ctx);

	/* Hand the slot back. The image outlives this instance whenever another
	 * Notepad is still running, so a slot not released here is gone for the
	 * rest of the boot.
	 */
	if (st != NULL) {
		st->used = false;
	}

	np_host->log(ctx, 0, "Notepad closed");
}

struct zd_zapp_manifest zd_zapp_manifest = {
	.magic = ZD_ZAPP_MAGIC,
	.abi_major = ZD_ABI_MAJOR,
	.abi_minor = ZD_ABI_MINOR,
	.flags = 0, /* untrusted: no unsafe_lvgl_content here either */
	.name = "Notepad",
	.icon = NULL,
	.init = notepad_init,
	.event = notepad_event,
	.fini = notepad_fini,
};
LL_EXTENSION_SYMBOL(zd_zapp_manifest);
