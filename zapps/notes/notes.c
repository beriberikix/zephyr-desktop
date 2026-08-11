/*
 * notes — a second zapp, and the one that exercises storage.
 *
 * Where hello is the minimum, this one uses the parts of the ABI hello does
 * not: path resolution, more than one window per instance, click events,
 * per-instance state, and -- since ABI 0.3 -- reading and writing a real file.
 *
 * It appends a line per click to $HOME/notes.txt and shows the file back on the
 * next launch, which is the smallest thing that proves the whole chain: a real
 * .llext, found on a real filesystem, calling through the permission shim to
 * write to real storage, and finding its work still there after a reboot.
 *
 * Note what is NOT included: no <string.h>, no <stdio.h>. The desktop exports
 * exactly one symbol to extensions (CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n), so a
 * zapp has no libc at all -- strlen and snprintf would link here and fail at
 * load. Hence the handful of tiny helpers below. That is the cost of a one
 * symbol export surface, and it is worth paying.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h> /* compiler-provided; no libc is linked in */

#include <zephyr/llext/symbol.h>

#include <zd/zapp_abi.h>

#define MAX_TRIES 8

/* One screenful. The window is small and the point is the round trip, not
 * paging: read the tail of the file and show what fits.
 */
#define SHOWN_BYTES 192
#define MAX_LINES   6
#define LINE_MAX    32

/* Rough height of one line of the content area's font, in pixels. A zapp has no
 * way to ask -- there is no text-metrics call in the ABI -- so it estimates and
 * the worst case is one line too many, clipped by the window.
 */
#define LINE_PX 16

/*
 * Per-instance state.
 *
 * Not file-scope variables: llext refcounts by name, so two instances of notes
 * share one image and therefore one copy of every static. Anything that must
 * differ between windows lives here and hangs off set_user_data().
 *
 * `host` below is the exception that is safe -- every instance writes the
 * identical vtable pointer.
 */
struct notes_state {
	bool used;
	/* Every window this instance owns, not just the first. They are views of
	 * one file, so a click in any of them appends and all of them update --
	 * which is also the cheapest visible proof that per-instance state is
	 * genuinely per-instance.
	 */
	zd_window_t wins[MAX_TRIES];
	zd_label_t bodies[MAX_TRIES];
	/* How many lines each window has room for. Per window, because they can
	 * now be resized independently -- one instance, several different views
	 * of the same file.
	 */
	unsigned int rows[MAX_TRIES];
	unsigned int count;
	char path[ZD_PATH_MAX];
	unsigned int lines;
	char shown[SHOWN_BYTES + 1];
};

static const struct zd_host_api *host;
static struct notes_state states[4];

/* --- the libc we do not have -------------------------------------------------- */

/*
 * Zero a block. Through a volatile pointer on purpose: given a plain loop -- or
 * a compound literal assignment, which is how this started -- GCC recognises
 * the pattern and emits a call to memset, and memset is not one of the symbols
 * a zapp is allowed to import. The extension then fails to load, at runtime,
 * with an undefined-symbol error a long way from the assignment that caused it.
 */
static void z_zero(void *dst, uint32_t len)
{
	volatile unsigned char *byte = dst;

	while (len-- > 0u) {
		*byte++ = 0u;
	}
}

static uint32_t z_len(const char *s)
{
	const char *p = s;

	while (*p != '\0') {
		p++;
	}

	return (uint32_t)(p - s);
}

/** Append @p src to @p dst. @return the new length, or 0 if it would not fit. */
static uint32_t z_append(char *dst, uint32_t at, uint32_t cap, const char *src)
{
	while (*src != '\0') {
		if (at + 1 >= cap) {
			return 0;
		}
		dst[at++] = *src++;
	}

	dst[at] = '\0';
	return at;
}

/** Append @p value in decimal. @return the new length, or 0 if it would not fit. */
static uint32_t z_append_u32(char *dst, uint32_t at, uint32_t cap, uint32_t value)
{
	char digits[10];
	int n = 0;

	do {
		digits[n++] = (char)('0' + (value % 10u));
		value /= 10u;
	} while (value != 0u);

	while (n > 0) {
		if (at + 1 >= cap) {
			return 0;
		}
		dst[at++] = digits[--n];
	}

	dst[at] = '\0';
	return at;
}

/* --- storage ------------------------------------------------------------------ */

/*
 * Append one line and flush it. Opened and closed per click rather than held
 * open: the per-instance quota is small, and a note that only reaches the disk
 * when the zapp happens to exit is not saved at all.
 */
static int append_line(zd_zapp_ctx_t ctx, struct notes_state *st, const char *line)
{
	zd_file_t file;
	int ret;

	ret = host->fs_open(ctx, st->path, ZD_O_WRITE | ZD_O_CREATE | ZD_O_APPEND, &file);
	if (ret != 0) {
		return ret;
	}

	ret = host->fs_write(ctx, file, line, z_len(line));
	if (ret >= 0) {
		ret = host->fs_sync(ctx, file);
	}

	host->fs_close(ctx, file);
	return ret;
}

/* Read the tail of the file into st->shown. */
static int load_tail(zd_zapp_ctx_t ctx, struct notes_state *st)
{
	struct zd_dirent info;
	zd_file_t file;
	int32_t from = 0;
	uint32_t got = 0;
	int ret;

	st->shown[0] = '\0';

	ret = host->fs_stat(ctx, st->path, &info);
	if (ret != 0) {
		return ret; /* no notes yet is the ordinary first-run case */
	}

	if (info.size > SHOWN_BYTES) {
		from = (int32_t)(info.size - SHOWN_BYTES);
	}

	ret = host->fs_open(ctx, st->path, ZD_O_READ, &file);
	if (ret != 0) {
		return ret;
	}

	ret = host->fs_seek(ctx, file, from, ZD_SEEK_SET);
	if (ret == 0) {
		/* Reads are allowed to be short by contract, so loop rather than
		 * assuming one call returns everything asked for.
		 */
		while (got < SHOWN_BYTES) {
			ret = host->fs_read(ctx, file, st->shown + got, SHOWN_BYTES - got);
			if (ret <= 0) {
				break;
			}
			got += (uint32_t)ret;
		}
	}

	host->fs_close(ctx, file);
	st->shown[got] = '\0';

	return ret < 0 ? ret : 0;
}

/* --- display ------------------------------------------------------------------- */

/* Keep only the last @p rows lines, so the label cannot outgrow the window. */
static const char *last_lines(const char *text, unsigned int rows)
{
	const char *start = text;
	unsigned int newlines = 0;

	for (const char *p = text + z_len(text); p > text; p--) {
		if (p[-1] != '\n') {
			continue;
		}

		newlines++;
		if (newlines > rows) {
			start = p;
			break;
		}
	}

	return start;
}

static void redraw(zd_zapp_ctx_t ctx, struct notes_state *st)
{
	for (unsigned int i = 0; i < st->count; i++) {
		const char *text = st->shown[0] != '\0'
					   ? last_lines(st->shown, st->rows[i])
					   : "(no notes yet -- click here)";

		host->label_set_text(ctx, st->bodies[i], text);
	}
}

/* Which of this instance's windows @p win is, or count if it is none of them. */
static unsigned int index_of(struct notes_state *st, zd_window_t win)
{
	unsigned int i;

	for (i = 0; i < st->count; i++) {
		if (st->wins[i] == win) {
			break;
		}
	}

	return i;
}

/* --- lifecycle ------------------------------------------------------------------ */

static struct notes_state *claim_state(void)
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

static int notes_init(zd_zapp_ctx_t ctx, const struct zd_host_api *api)
{
	struct notes_state *st;
	char home[ZD_PATH_MAX];
	uint32_t at;

	host = api;

	st = claim_state();
	if (st == NULL) {
		return -1;
	}

	api->set_user_data(ctx, st);

	/* Keep asking until the desktop refuses, so the per-zapp window quota is
	 * visible in the running system rather than only asserted in a test.
	 */
	for (unsigned int i = 0; i < MAX_TRIES; i++) {
		struct zd_window_desc desc = {
			.title = "Notes",
			.geom = { 0, 0, 0, 0 },
		};
		zd_window_t win = api->window_create(ctx, &desc);

		if (win == NULL) {
			break; /* quota reached -- expected, not an error */
		}

		st->wins[st->count] = win;
		st->bodies[st->count] = api->label_create(ctx, win, "", 6, 8);
		/* A guess until the first ZD_EV_RESIZED corrects it. There is no
		 * call to ask how big a content area is at creation time, and
		 * the zapp must not work it out from the frame -- the chrome's
		 * dimensions are the desktop's business.
		 */
		st->rows[st->count] = MAX_LINES;
		st->count++;
	}

	if (st->count == 0) {
		st->used = false;
		return -1;
	}

	/* The zapp has no idea what the filesystem root is called; that is the
	 * session's business. It asks.
	 */
	if (api->path_resolve(ctx, ZD_DIR_HOME, home, sizeof(home)) != 0) {
		api->log(ctx, 0, "cannot resolve home");
		st->used = false;
		return -1;
	}

	at = z_append(st->path, 0, sizeof(st->path), home);
	at = at != 0 ? z_append(st->path, at, sizeof(st->path), "/notes.txt") : 0;
	if (at == 0) {
		st->used = false;
		return -1;
	}

	api->log(ctx, 0, st->path);

	(void)load_tail(ctx, st);
	redraw(ctx, st);

	return 0;
}

/*
 * The desktop is asking, not telling: flush and then say yes.
 *
 * There is nothing here to warn about -- each click is already synced to the
 * card before the label is redrawn -- so the honest answer is to close at once
 * rather than to sit on the grace period looking thoughtful. A zapp that did
 * have unsaved state would write it here, and would still have to close: the
 * desktop takes the window either way once CONFIG_ZD_CLOSE_GRACE_MS is up.
 */
static void on_close_request(zd_zapp_ctx_t ctx, struct notes_state *st, zd_window_t win)
{
	unsigned int i = index_of(st, win);

	host->log(ctx, 0, "close requested; nothing unsaved");

	/* Forget the window before asking for it to go. The close is deferred --
	 * the reap does the deleting after this callback returns -- so a redraw
	 * between now and then would be drawing into a window on its way out.
	 */
	if (i < st->count) {
		st->wins[i] = st->wins[st->count - 1];
		st->bodies[i] = st->bodies[st->count - 1];
		st->rows[i] = st->rows[st->count - 1];
		st->count--;
	}

	host->window_close(ctx, win);
}

static void notes_event(zd_zapp_ctx_t ctx, const struct zd_event *ev)
{
	struct notes_state *st = host->get_user_data(ctx);
	char line[LINE_MAX];
	unsigned int index;
	uint32_t at;

	if (st == NULL) {
		return;
	}

	/* A zapp is handed events for its own windows only, but an instance with
	 * several must still tell them apart.
	 */
	index = index_of(st, ev->win);

	switch (ev->type) {
	case ZD_EV_WINDOW_CLOSE_REQUEST:
		on_close_request(ctx, st, ev->win);
		return;

	case ZD_EV_RESIZED:
		/* Show as much of the file as now fits. This is the visible
		 * proof that the event arrives with the *content* area's size:
		 * the zapp divides by a line height and nothing else.
		 */
		if (index < st->count) {
			unsigned int rows = (unsigned int)ev->resize.h / LINE_PX;

			st->rows[index] = rows > 0u ? rows : 1u;
			redraw(ctx, st);
		}
		return;

	case ZD_EV_MINIMIZED:
		host->log(ctx, 0, "minimised; nothing to draw");
		return;

	case ZD_EV_RESTORED:
		/* Back on screen, and the file may have moved on while this
		 * window was away -- another instance shares it.
		 */
		(void)load_tail(ctx, st);
		redraw(ctx, st);
		return;

	case ZD_EV_CLICK:
		break;

	default:
		return;
	}

	if (index >= st->count) {
		return;
	}

	at = z_append(line, 0, sizeof(line), "note ");
	at = at != 0 ? z_append_u32(line, at, sizeof(line), ++st->lines) : 0;
	at = at != 0 ? z_append(line, at, sizeof(line), " at ") : 0;
	at = at != 0 ? z_append_u32(line, at, sizeof(line),
				    (uint32_t)host->uptime_ms()) : 0;
	at = at != 0 ? z_append(line, at, sizeof(line), "ms\n") : 0;
	if (at == 0) {
		return;
	}

	if (append_line(ctx, st, line) < 0) {
		host->log(ctx, 0, "could not append -- is the volume writable?");
		return;
	}

	(void)load_tail(ctx, st);
	redraw(ctx, st);
}

static void notes_fini(zd_zapp_ctx_t ctx)
{
	struct notes_state *st = host->get_user_data(ctx);

	/* Hand the slot back. The image outlives this instance whenever another
	 * copy of notes is still running, so a slot not released here is gone
	 * until the last instance unloads.
	 */
	if (st != NULL) {
		st->used = false;
	}

	host->log(ctx, 0, "notes closed");
}

struct zd_zapp_manifest zd_zapp_manifest = {
	.magic = ZD_ZAPP_MAGIC,
	.abi_major = ZD_ABI_MAJOR,
	.abi_minor = ZD_ABI_MINOR,
	.flags = 0,
	.name = "Notes",
	.icon = NULL,
	.init = notes_init,
	.event = notes_event,
	.fini = notes_fini,
};
LL_EXTENSION_SYMBOL(zd_zapp_manifest);
