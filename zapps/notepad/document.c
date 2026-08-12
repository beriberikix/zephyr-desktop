/*
 * notepad — the document: load, save, and what the titlebar says about it.
 *
 * Everything here goes through the ABI and nothing else. No libc, no Zephyr, no
 * idea what a filesystem is beyond "the desktop will open a path I built from
 * path_resolve()".
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "notepad.h"

/*
 * One transfer buffer, shared by load and save.
 *
 * File-scope, which is normally the trap in a zapp -- llext loads an image once
 * and every instance shares its .bss, so two Notepads would share this. That is
 * safe here and only here: it is used within a single call, never held across
 * one, and a zapp cannot be re-entered mid-call because the desktop dispatches
 * events one at a time on its own thread. Anything that outlives a call lives
 * in struct np_state, which is per instance.
 */
#define CHUNK 256
static char io_buf[CHUNK + 1];

/*
 * Say how much moved.
 *
 * Not decoration: a zapp is otherwise a black box from the console, and "the
 * window did not complain" is not the same evidence as "eleven bytes came
 * back". This is what a headless run has to go on.
 */
static void say_bytes(zd_zapp_ctx_t ctx, const char *what, uint32_t bytes)
{
	char msg[32];
	uint32_t at;

	at = z_append(msg, 0, sizeof(msg), what);
	at = at != 0 ? z_append_u32(msg, at, sizeof(msg), bytes) : 0;
	at = at != 0 ? z_append(msg, at, sizeof(msg), " bytes") : 0;

	if (at != 0) {
		np_host->log(ctx, 0, msg);
	}
}

void np_doc_new(zd_zapp_ctx_t ctx, struct np_state *st)
{
	np_host->text_set_text(ctx, st->text, "");
	st->path[0] = '\0';
	st->dirty = false;
	np_doc_retitle(ctx, st);
}

int np_doc_open(zd_zapp_ctx_t ctx, struct np_state *st, const char *path)
{
	struct zd_dirent info;
	zd_file_t file;
	uint32_t total = 0;
	int ret;

	ret = np_host->fs_stat(ctx, path, &info);
	if (ret != 0) {
		return ret;
	}

	/*
	 * Refuse rather than truncate.
	 *
	 * A text widget holds a bounded document, and opening the first few
	 * kilobytes of a bigger file would look like success right up until the
	 * user saved it back and discovered they had deleted the rest. Notepad
	 * 1.0 put up "This file is too large for Notepad" for exactly this
	 * reason.
	 *
	 * The bound is asked for, not assumed. It is the desktop's number and
	 * it differs between boards; a zapp that hardcoded it would refuse files
	 * it could have opened, or accept ones it will quietly cut short.
	 */
	if (info.size > (uint32_t)np_host->text_get_capacity(ctx, st->text)) {
		return NP_E_TOO_BIG;
	}

	ret = np_host->fs_open(ctx, path, ZD_O_READ, &file);
	if (ret != 0) {
		return ret;
	}

	np_host->text_set_text(ctx, st->text, "");

	/* Reads are short by contract, so loop. Appending each chunk into the
	 * widget rather than assembling the whole file first is what keeps this
	 * zapp's memory to one 256-byte buffer.
	 */
	while (true) {
		int got = np_host->fs_read(ctx, file, io_buf, CHUNK);

		if (got <= 0) {
			ret = got;
			break;
		}

		io_buf[got] = '\0';
		np_host->text_insert(ctx, st->text, io_buf);
		total += (uint32_t)got;
	}

	np_host->fs_close(ctx, file);

	if (ret < 0) {
		return ret;
	}

	np_host->text_set_cursor(ctx, st->text, 0);
	(void)z_strcpy(st->path, sizeof(st->path), path);
	st->dirty = false;
	np_doc_retitle(ctx, st);
	say_bytes(ctx, "opened ", total);

	return 0;
}

int np_doc_save(zd_zapp_ctx_t ctx, struct np_state *st)
{
	zd_file_t file;
	uint32_t at = 0;
	int ret;

	if (st->path[0] == '\0') {
		return NP_E_NO_PATH; /* Save As should have run first */
	}

	ret = np_host->fs_open(ctx, st->path,
			       ZD_O_WRITE | ZD_O_CREATE | ZD_O_TRUNC, &file);
	if (ret != 0) {
		return ret;
	}

	while (true) {
		int got = np_host->text_get_text(ctx, st->text, at, io_buf, sizeof(io_buf));
		int put;

		if (got <= 0) {
			ret = got;
			break;
		}

		/* Writes are allowed to be short too, so a chunk may take more
		 * than one call. Getting this wrong loses the tail of every
		 * document larger than one transfer and looks fine on a small
		 * one, which is the worst way for it to be wrong.
		 */
		for (int done = 0; done < got; done += put) {
			put = np_host->fs_write(ctx, file, io_buf + done,
						(uint32_t)(got - done));
			if (put <= 0) {
				np_host->fs_close(ctx, file);
				return put < 0 ? put : NP_E_SHORT_WRITE;
			}
		}

		at += (uint32_t)got;
	}

	if (ret == 0) {
		ret = np_host->fs_sync(ctx, file);
	}

	np_host->fs_close(ctx, file);

	if (ret != 0) {
		return ret;
	}

	st->dirty = false;
	np_doc_retitle(ctx, st);
	say_bytes(ctx, "saved ", at);

	return 0;
}

void np_doc_retitle(zd_zapp_ctx_t ctx, struct np_state *st)
{
	char title[ZD_TITLE_MAX];
	uint32_t at = 0;

	/*
	 * "*name - Notepad", where the star is the modified marker.
	 *
	 * The original put the filename first and no marker at all -- it asked
	 * on the way out instead. A star costs one character and means the
	 * taskbar button says so too, which matters far more on a screen where
	 * the window may be behind something else.
	 */
	if (st->dirty) {
		at = z_append(title, at, sizeof(title), "*");
	}

	at = z_append(title, at, sizeof(title),
		      st->path[0] != '\0' ? z_basename(st->path) : "Untitled");

	/* Truncated to fit is fine for the suffix; the name is the part that
	 * has to survive, which is why it goes on first.
	 */
	if (at != 0) {
		(void)z_append(title, at, sizeof(title), " - Notepad");
	}

	np_host->window_set_title(ctx, st->win, title);
}
