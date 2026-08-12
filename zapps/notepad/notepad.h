/*
 * notepad — shared state and command ids.
 *
 * Split across four files because it can be: the ARM targets moved to
 * LLEXT_TYPE_ELF_RELOCATABLE in milestone K, and this is the first zapp that
 * would have been unpleasant as one translation unit. That is not an accident
 * of taste -- the design doc's risk #6 named the text editor as the thing that would
 * hit the one-file wall, and it did.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZAPPS_NOTEPAD_NOTEPAD_H_
#define ZAPPS_NOTEPAD_NOTEPAD_H_

#include <stdbool.h>

#include <lib/zapplib.h>
#include <zd/zapp_abi.h>

/*
 * How many Notepads can run at once, from this zapp's side.
 *
 * Its own number, deliberately not CONFIG_ZD_MAX_CLIENTS. The desktop's Kconfig
 * happens to be reachable here -- zapps compile with autoconf.h -- and reaching
 * for it would weld this zapp to one desktop build and break the out-of-tree
 * story the ABI header exists to protect. The desktop enforces its own limits
 * and will simply refuse the window.
 */
#define NP_MAX_INSTANCES 4

/*
 * Notepad's own failures, in the negative-integer space the ABI uses.
 *
 * Deliberately not errno values. A zapp has no libc, and while <errno.h> is
 * only macros and would compile, borrowing the desktop's error space to say
 * something the desktop never said is a lie about where the failure came from.
 * These three are ours; anything else negative came back from a host call.
 */
#define NP_E_TOO_BIG     -1000
#define NP_E_NO_PATH     -1001
#define NP_E_SHORT_WRITE -1002

/* Menu command ids. Ours alone; the desktop only hands them back. */
#define NP_FILE_NEW    1
#define NP_FILE_OPEN   2
#define NP_FILE_SAVE   3
#define NP_FILE_SAVEAS 4
#define NP_FILE_EXIT   5

#define NP_EDIT_CUT    10
#define NP_EDIT_COPY   11
#define NP_EDIT_PASTE  12
#define NP_EDIT_DELETE 13
#define NP_EDIT_SELALL 14

/* Dialog ids, in the same namespace sense: ours. */
#define NP_DLG_SAVE_CHANGES 1
#define NP_DLG_OPEN         2
#define NP_DLG_SAVEAS       3

/**
 * What to do once the user has answered "the text has changed -- save it?".
 *
 * The whole reason this exists: every one of New, Open and Exit has to stop,
 * ask, and then carry on doing something different depending on the answer,
 * and the answer arrives in a later event with no memory of what asked.
 */
enum np_pending {
	NP_PENDING_NONE,
	NP_PENDING_NEW,
	NP_PENDING_OPEN,
	NP_PENDING_EXIT,
};

struct np_state {
	bool used;
	zd_window_t win;
	zd_text_t text;
	zd_menu_t file_menu;
	zd_menu_t edit_menu;

	char path[ZD_PATH_MAX]; /**< empty means Untitled */
	bool dirty;
	enum np_pending pending;
};

/** The host table. One per image, and every instance writes the same value. */
extern const struct zd_host_api *np_host;

/* --- document.c ------------------------------------------------------------- */

/** Empty the buffer and forget the path. */
void np_doc_new(zd_zapp_ctx_t ctx, struct np_state *st);

/** Read @p path into the text widget. @return 0, or a negative errno. */
int np_doc_open(zd_zapp_ctx_t ctx, struct np_state *st, const char *path);

/** Write the text widget to st->path. @return 0, or a negative errno. */
int np_doc_save(zd_zapp_ctx_t ctx, struct np_state *st);

/** Retitle the window from the path and the dirty flag. */
void np_doc_retitle(zd_zapp_ctx_t ctx, struct np_state *st);

/* --- commands.c ------------------------------------------------------------- */

/** Run a menu command, or the accelerator that means the same thing. */
void np_command(zd_zapp_ctx_t ctx, struct np_state *st, uint16_t id);

/** Handle an answered dialog. */
void np_dialog_answered(zd_zapp_ctx_t ctx, struct np_state *st, uint16_t id,
			int16_t result);

/** Turn a CTRL-modified key into a command, or 0 if it is not one. */
uint16_t np_accelerator(uint32_t code, uint32_t unicode, uint16_t mods);

/** Ask about unsaved work, then do @p next. */
void np_maybe_discard(zd_zapp_ctx_t ctx, struct np_state *st, enum np_pending next);

#endif /* ZAPPS_NOTEPAD_NOTEPAD_H_ */
