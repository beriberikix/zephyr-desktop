/*
 * notepad — what the menu items and the accelerators actually do.
 *
 * The interesting part of this file is not any single command; it is that three
 * of them (New, Open, Exit) cannot simply happen. They have to stop, ask "the
 * text has changed -- save it?", and then carry on doing something that depends
 * on an answer arriving in a later, entirely separate event. st->pending is the
 * one piece of state that makes that possible, and everything awkward here is
 * downstream of the desktop being asynchronous -- which it is on purpose, since
 * a modal loop on the desktop thread would stop the dialog drawing.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "notepad.h"

static void say(zd_zapp_ctx_t ctx, const char *what)
{
	np_host->log(ctx, 0, what);
}

/** Report a failed file operation to the user, since there is nowhere else. */
static void complain(zd_zapp_ctx_t ctx, const char *what, int err)
{
	char msg[64];
	uint32_t at;

	at = z_append(msg, 0, sizeof(msg), what);

	if (err == NP_E_TOO_BIG) {
		at = at != 0 ? z_append(msg, at, sizeof(msg),
					"\n\nThis file is too large for Notepad.")
			     : 0;
	} else {
		at = at != 0 ? z_append(msg, at, sizeof(msg), "\n\nError ") : 0;
		at = at != 0 ? z_append_u32(msg, at, sizeof(msg), (uint32_t)(-err)) : 0;
		at = at != 0 ? z_append(msg, at, sizeof(msg), ".") : 0;
	}

	if (at != 0) {
		(void)np_host->dialog_confirm(ctx, "Notepad", msg, ZD_DLG_OK_CANCEL, 0);
	}

	say(ctx, msg);
}

/* --- the pending dance ---------------------------------------------------------- */

static void run_pending(zd_zapp_ctx_t ctx, struct np_state *st)
{
	enum np_pending what = st->pending;

	st->pending = NP_PENDING_NONE;

	switch (what) {
	case NP_PENDING_NEW:
		np_doc_new(ctx, st);
		break;

	case NP_PENDING_OPEN:
		(void)np_host->dialog_file(ctx, "Open", ZD_DIR_HOME, ZD_DLG_OPEN,
					   NP_DLG_OPEN);
		break;

	case NP_PENDING_EXIT:
		/* The window goes away for good here. The desktop's reap does
		 * the deleting after this callback returns, so nothing below
		 * may touch st->win or st->text again -- which is why this is
		 * the last statement and why the state is cleared first.
		 */
		st->text = NULL;
		np_host->window_close(ctx, st->win);
		break;

	case NP_PENDING_NONE:
		break;
	}
}

void np_maybe_discard(zd_zapp_ctx_t ctx, struct np_state *st, enum np_pending next)
{
	st->pending = next;

	if (!st->dirty) {
		run_pending(ctx, st);
		return;
	}

	/*
	 * Nearly the original's words. The desktop gives it a bounded grace
	 * period to answer a close request, and would normally take the window
	 * when that expires -- but it will not while this dialog is up, because
	 * a zapp that is asking the user is doing what it was asked, not
	 * ignoring it. See zd_zapp_on_client_close_stalled().
	 */
	(void)np_host->dialog_confirm(ctx, "Notepad",
				      "The text in the file has changed.\n\nSave it?",
				      ZD_DLG_YES_NO_CANCEL, NP_DLG_SAVE_CHANGES);
}

/* --- saving --------------------------------------------------------------------- */

/** Save, asking for a name first if there is not one yet. @return true if saved. */
static bool save_or_ask(zd_zapp_ctx_t ctx, struct np_state *st)
{
	int ret;

	if (st->path[0] == '\0') {
		(void)np_host->dialog_file(ctx, "Save As", ZD_DIR_HOME, ZD_DLG_SAVE,
					   NP_DLG_SAVEAS);
		return false; /* the answer arrives later and picks up st->pending */
	}

	ret = np_doc_save(ctx, st);
	if (ret != 0) {
		complain(ctx, "Cannot save this file.", ret);
		return false;
	}

	return true;
}

/* --- commands ------------------------------------------------------------------- */

/* An edit the zapp made itself. The desktop does not send
 * ZD_EV_TEXT_CHANGED for those -- deliberately, so a zapp is not re-entered
 * from inside its own host call -- so the dirty flag is ours to set.
 */
static void mark_dirty(zd_zapp_ctx_t ctx, struct np_state *st, int moved)
{
	if (moved <= 0) {
		return; /* nothing selected, or an empty clipboard */
	}

	st->dirty = true;
	np_doc_retitle(ctx, st);
}

void np_command(zd_zapp_ctx_t ctx, struct np_state *st, uint16_t id)
{
	switch (id) {
	case NP_FILE_NEW:
		np_maybe_discard(ctx, st, NP_PENDING_NEW);
		break;

	case NP_FILE_OPEN:
		np_maybe_discard(ctx, st, NP_PENDING_OPEN);
		break;

	case NP_FILE_SAVE:
		(void)save_or_ask(ctx, st);
		break;

	case NP_FILE_SAVEAS:
		(void)np_host->dialog_file(ctx, "Save As", ZD_DIR_HOME, ZD_DLG_SAVE,
					   NP_DLG_SAVEAS);
		break;

	case NP_FILE_EXIT:
		np_maybe_discard(ctx, st, NP_PENDING_EXIT);
		break;

	case NP_EDIT_CUT:
		mark_dirty(ctx, st, np_host->text_cut(ctx, st->text));
		break;

	case NP_EDIT_COPY:
		(void)np_host->text_copy(ctx, st->text); /* changes nothing */
		break;

	case NP_EDIT_PASTE:
		mark_dirty(ctx, st, np_host->text_paste(ctx, st->text));
		break;

	case NP_EDIT_DELETE:
		mark_dirty(ctx, st, np_host->text_delete_selection(ctx, st->text));
		break;

	case NP_EDIT_SELALL:
		(void)np_host->text_select(ctx, st->text, 0,
					   (uint32_t)np_host->text_get_length(ctx, st->text));
		break;

	default:
		break;
	}
}

/* --- dialogs -------------------------------------------------------------------- */

void np_dialog_answered(zd_zapp_ctx_t ctx, struct np_state *st, uint16_t id,
			int16_t result)
{
	char path[ZD_PATH_MAX];
	int ret;

	switch (id) {
	case NP_DLG_SAVE_CHANGES:
		if (result == ZD_DLG_YES) {
			/* Save first, then carry on -- but if there is no name
			 * yet, save_or_ask() has put up Save As and st->pending
			 * survives to be picked up when THAT is answered. Two
			 * dialogs deep, and the state machine is the only reason
			 * it works.
			 */
			if (save_or_ask(ctx, st)) {
				run_pending(ctx, st);
			}
		} else if (result == ZD_DLG_NO) {
			run_pending(ctx, st);
		} else {
			/*
			 * Cancel. Say so out loud, or the window goes anyway.
			 *
			 * The desktop gave a bounded grace period to answer the
			 * close request; letting it lapse is how a wedged zapp
			 * loses its window, and doing nothing here would be
			 * indistinguishable from being wedged. Declining is the
			 * answer, and the next click on the close box asks
			 * again from the beginning.
			 */
			if (st->pending == NP_PENDING_EXIT) {
				np_host->window_close_cancel(ctx, st->win);
			}
			st->pending = NP_PENDING_NONE;
			say(ctx, "close cancelled");
		}
		break;

	case NP_DLG_OPEN:
		if (result != ZD_DLG_OK) {
			break;
		}
		if (np_host->dialog_get_path(ctx, path, sizeof(path)) < 0) {
			break;
		}
		ret = np_doc_open(ctx, st, path);
		if (ret != 0) {
			complain(ctx, "Cannot open this file.", ret);
		}
		break;

	case NP_DLG_SAVEAS:
		if (result != ZD_DLG_OK) {
			st->pending = NP_PENDING_NONE;
			break;
		}
		if (np_host->dialog_get_path(ctx, path, sizeof(path)) < 0) {
			break;
		}

		(void)z_strcpy(st->path, sizeof(st->path), path);

		ret = np_doc_save(ctx, st);
		if (ret != 0) {
			st->path[0] = '\0';
			st->pending = NP_PENDING_NONE;
			complain(ctx, "Cannot save this file.", ret);
			break;
		}

		/* Whatever was waiting on the save can happen now. */
		run_pending(ctx, st);
		break;

	default:
		break;
	}
}

/* --- accelerators ---------------------------------------------------------------- */

uint16_t np_accelerator(uint32_t code, uint32_t unicode, uint16_t mods)
{
	if (code != ZD_KEY_CHAR || (mods & ZD_MOD_CTRL) == 0) {
		return 0;
	}

	/* Lower case only: the desktop reports the character the key produced,
	 * and Ctrl+Shift+S produces 'S'. Treating that as Save would be wrong
	 * the day Ctrl+Shift+S means something else.
	 */
	switch (unicode) {
	case 'n': return NP_FILE_NEW;
	case 'o': return NP_FILE_OPEN;
	case 's': return NP_FILE_SAVE;
	case 'x': return NP_EDIT_CUT;
	case 'c': return NP_EDIT_COPY;
	case 'v': return NP_EDIT_PASTE;
	case 'a': return NP_EDIT_SELALL;
	default:  return 0;
	}
}
