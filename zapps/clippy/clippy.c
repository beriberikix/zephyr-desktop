/*
 * clippy — "It looks like you're writing an RTOS."
 *
 * Asks a Hugging Face Space a question about Zephyr and shows the answer. It
 * is the first zapp that waits on something the desktop does not control, and
 * that is the whole reason it exists: everything through ABI 0.7 either
 * answered immediately or was a timer. This one starts work that finishes
 * tens of seconds later, on a thread it never sees, and has to stay a good
 * citizen of the frame loop while it does.
 *
 * What that costs, concretely:
 *
 *   - The request is fire-and-forget. http_request() returns an id, not an
 *     answer. Nothing here blocks, because a zapp runs inside the desktop's
 *     draw loop and a blocking call would stop the clock and every other
 *     window along with it.
 *   - The answer arrives as ZD_EV_HTTP_RESPONSE with ev->win == NULL, so the
 *     usual `ev->win != mine` guard would swallow it. Type is checked first,
 *     the same trap ZD_EV_TIMER set in 0.7.
 *   - The body is not in the event. It is read out with http_read() and then
 *     released, so the desktop can reuse the buffer.
 *
 * Per-instance state lives behind set_user_data(). llext loads this image once
 * and refcounts it, so a file-scope `static struct state` would be shared
 * between two Clippys and the second would overwrite the first.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/llext/symbol.h>

#include <zd/zapp_abi.h>

#include "../lib/zapplib.h"

/* Where the answers come from. Plain http on purpose: the desktop has no TLS
 * stack, so tools/hf-proxy.py on the development host terminates TLS and
 * collapses the Space's two-step Gradio call into one plain-text reply. That
 * also means this zapp needs no JSON parser, which matters a great deal
 * without a libc. See docs/clippy.md.
 *
 * 10.0.2.2 is the QEMU user-mode alias for the host. A real device would point
 * this somewhere reachable on its own network.
 */
#define ASK_URL "http://10.0.2.2:8080/ask"

#define QUESTION_MAX 256
#define ANSWER_MAX   2048

/* Animation cadence. Fast enough to read as motion, slow enough that a
 * paperclip is not competing with the desktop for frames.
 */
#define TICK_MS  400
#define TIMER_ID 1

/* Per instance, not per image. Two Clippys share this file's .bss, so a
 * file-scope struct would have the second window overwriting the first's
 * conversation. Same pool idiom as mines.
 */
#define CLIPPY_MAX_INSTANCES 2

/* What the paperclip is doing. Mapped onto ZD_ICON_STATE_* in draw_face(); kept
 * as its own enum because the two are not the same list -- CLIPPY_IDLE blinks,
 * which is two icon states alternating, and CLIPPY_DONE is just NORMAL.
 */
enum clippy_mood {
	CLIPPY_IDLE = 0,
	CLIPPY_BUSY,
	CLIPPY_DONE,
	CLIPPY_SAD,
};

struct state {
	bool used;
	zd_window_t win;
	zd_balloon_t bubble; /* the paperclip and what it is saying */
	zd_text_t answer;    /* what came back, in full and scrollable */
	zd_text_t entry;     /* the question being typed */

	enum clippy_mood mood;
	uint32_t tick;
	uint32_t started_ms;

	bool waiting;
	uint16_t request; /* valid only while waiting */

	char question[QUESTION_MAX];
	char scratch[ANSWER_MAX];
};

static const struct zd_host_api *host;

/* There is no allocator in the ABI -- deliberately, so a zapp cannot exhaust
 * the desktop's heap. State comes from a fixed pool, claimed at init and
 * released at fini.
 */
static struct state states[CLIPPY_MAX_INSTANCES];

static struct state *claim_state(void)
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

/* One instance's state. See the note above about why this is not simply a
 * file-scope struct: two Clippys share this image's .bss.
 */
static struct state *state_of(zd_zapp_ctx_t ctx)
{
	return (struct state *)host->get_user_data(ctx);
}

/*
 * Push the mood at the desktop, which owns the artwork.
 *
 * 0.9 made the paperclip a stock icon rather than a picture this zapp carries.
 * That is the whole reason there is no longer a face.c here: a zapp cannot draw
 * -- no framebuffer, no lv_obj_t, and with LLEXT_EXPORT_DEFAULT_GROUPS=n not
 * one LVGL symbol to link against -- so what used to be five frames of ASCII
 * art is now a number.
 *
 * Idle blinks: three ticks open, one shut. A paperclip that blinks evenly looks
 * like it is malfunctioning rather than waiting.
 */
static void draw_face(zd_zapp_ctx_t ctx, struct state *st)
{
	uint32_t icon_state = ZD_ICON_STATE_NORMAL;

	switch (st->mood) {
	case CLIPPY_BUSY:
		icon_state = ZD_ICON_STATE_BUSY;
		break;
	case CLIPPY_SAD:
		icon_state = ZD_ICON_STATE_SAD;
		break;
	case CLIPPY_IDLE:
		if ((st->tick & 3u) == 3u) {
			icon_state = ZD_ICON_STATE_BLINK;
		}
		break;
	case CLIPPY_DONE:
	default:
		break;
	}

	host->balloon_set_icon(ctx, st->bubble, ZD_ICON_CLIP, icon_state);
}

/**
 * What the paperclip is saying: short, and in the balloon.
 *
 * Split from the answer on purpose. A balloon wraps but does not scroll, and an
 * answer from the documentation index runs to a couple of thousand characters
 * -- so the balloon carries the status and the text widget below it carries the
 * prose. Which also means the thing the user reads while waiting is the thing
 * that changes, rather than a wall of text with one line moving in it.
 */
static void say(zd_zapp_ctx_t ctx, struct state *st, const char *text)
{
	host->balloon_set_text(ctx, st->bubble, text);
}

/** The long form, in the scrollable widget underneath. */
static void show_answer(zd_zapp_ctx_t ctx, struct state *st, const char *text)
{
	host->text_set_text(ctx, st->answer, text);
}

static void say_waiting(zd_zapp_ctx_t ctx, struct state *st)
{
	uint32_t at = 0;
	/* Narrow to 32 bits before dividing. A 64-bit divide is one instruction
	 * on arm64 and a call to libgcc's __divdi3 on Xtensa, which builds
	 * cleanly and then fails to load on the CoreS3. The project has been
	 * bitten by exactly this.
	 */
	/* Narrowed to 32 bits before the subtract, not after: uptime_ms()
	 * returns int64_t, and a 64-bit divide is one instruction on arm64 but a
	 * call to libgcc's __divdi3 on Xtensa, which builds cleanly and then
	 * fails to load on the CoreS3. Everything after this line is 32-bit.
	 */
	uint32_t now = (uint32_t)host->uptime_ms();
	uint32_t elapsed = (now - st->started_ms) / 1000U;

	at = z_strcpy(st->scratch, sizeof(st->scratch), "Let me look that up... ");
	at = z_append_u32(st->scratch, at, sizeof(st->scratch), elapsed);
	at = z_append(st->scratch, at, sizeof(st->scratch), "s");
	(void)at;
	say(ctx, st, st->scratch);
}

static void ask(zd_zapp_ctx_t ctx, struct state *st)
{
	int rc;

	if (st->waiting) {
		return; /* one question at a time; the slot is still held */
	}

	/* Read the question straight out of the entry widget rather than
	 * tracking it on every keystroke: the widget is already the truth.
	 */
	rc = host->text_get_text(ctx, st->entry, 0, st->question, sizeof(st->question));
	if (rc <= 0 || st->question[0] == '\0') {
		st->mood = CLIPPY_IDLE;
		draw_face(ctx, st);
		say(ctx, st, "Type a question first.");
		return;
	}

	rc = host->http_request(ctx, ASK_URL, st->question, &st->request);
	if (rc != 0) {
		st->mood = CLIPPY_SAD;
		draw_face(ctx, st);
		/* The ABI documents -ENOSYS, -EBUSY and -EINVAL here, but a zapp
		 * cannot name them: zapp_abi.h includes only stdbool, stddef and
		 * stdint, and there is no errno.h to link against. So the message
		 * covers the causes rather than decoding the number.
		 */
		say(ctx, st, "I could not send that.");
		show_answer(ctx, st,
			    "Either this desktop was built without CONFIG_ZD_NET,\n"
			    "or too many requests are already in flight.\n\n"
			    "A build with CONFIG_ZD_CLIPPY=y and no networking gets\n"
			    "exactly this, on purpose: it is how this interface is\n"
			    "worked on without a TCP stack.");
		return;
	}

	st->waiting = true;
	st->started_ms = (uint32_t)host->uptime_ms();
	st->mood = CLIPPY_BUSY;
	st->tick = 0;
	draw_face(ctx, st);
	say_waiting(ctx, st);
}

static void answered(zd_zapp_ctx_t ctx, struct state *st, const struct zd_event *ev)
{
	int got;

	st->waiting = false;

	if (ev->http.status != 200) {
		st->mood = CLIPPY_SAD;
		draw_face(ctx, st);

		uint32_t at = z_strcpy(st->scratch, sizeof(st->scratch), "Status: ");
		/* Negative status is an errno rather than an HTTP code; say so
		 * instead of printing a meaningless "-113".
		 */
		if (ev->http.status < 0) {
			at = z_append(st->scratch, at, sizeof(st->scratch),
				      "the request never reached the proxy.\n\n"
				      "Start it with:\n  python tools/hf-proxy.py");
		} else {
			at = z_append_u32(st->scratch, at, sizeof(st->scratch),
					  (uint32_t)ev->http.status);
			at = z_append(st->scratch, at, sizeof(st->scratch),
				      "\n\nThe proxy answered, but not with an answer.");
		}
		(void)at;
		say(ctx, st, "That did not work.");
		show_answer(ctx, st, st->scratch);
		host->http_release(ctx, ev->http.id);
		return;
	}

	got = host->http_read(ctx, ev->http.id, 0, st->scratch, sizeof(st->scratch));
	/* Released as soon as the bytes are copied out. Holding it longer holds
	 * a slot and its whole body buffer for no reason.
	 */
	host->http_release(ctx, ev->http.id);

	if (got <= 0) {
		st->mood = CLIPPY_SAD;
		draw_face(ctx, st);
		say(ctx, st, "The proxy answered with nothing.");
		return;
	}

	st->mood = CLIPPY_DONE;
	draw_face(ctx, st);
	say(ctx, st, "Here you go.");
	show_answer(ctx, st, st->scratch);
}

static int clippy_init(zd_zapp_ctx_t ctx, const struct zd_host_api *api)
{
	struct zd_window_desc desc = {
		.title = "Clippy",
		.geom = { 0, 0, 320, 240 },
	};
	struct state *st;
	/* The balloon is as wide as the window and tall enough for the icon,
	 * which the desktop drops if it is given less. Everything else stacks
	 * under it. Fixed numbers rather than window_get_content_size(): the
	 * window is created at a size this zapp chose two lines up, and asking
	 * the desktop to measure something to find out what was just asked for
	 * is the read-back trap wm.h warns about.
	 */
	struct zd_rect bubble_at = { 4, 4, 304, 68 };
	struct zd_rect entry_at = { 4, 76, 300, 26 };
	struct zd_rect answer_at = { 4, 106, 300, 98 };

	host = api;

	if (api->abi_major != ZD_ABI_MAJOR || api->abi_minor < 9) {
		return -1; /* http_request in 0.8, the balloon in 0.9 */
	}

	/* z_zero rather than a struct assignment: GCC turns `*st = (struct
	 * state){0}` into a memset call, which is not there to link against.
	 * claim_state() does it.
	 */
	st = claim_state();
	if (st == NULL) {
		return -1; /* already two Clippys; one more is not worth a heap */
	}

	st->win = api->window_create(ctx, &desc);
	if (st->win == NULL) {
		return -1;
	}

	st->bubble = api->balloon_create(ctx, st->win, bubble_at.x, bubble_at.y,
					 bubble_at.w, bubble_at.h, ZD_ICON_CLIP);
	st->entry = api->text_create(ctx, st->win, &entry_at, ZD_TEXT_ONE_LINE);
	st->answer = api->text_create(ctx, st->win, &answer_at, ZD_TEXT_READONLY);
	if (st->bubble == NULL || st->entry == NULL || st->answer == NULL) {
		return -1;
	}

	api->set_user_data(ctx, st);

	st->mood = CLIPPY_IDLE;
	draw_face(ctx, st);
	api->text_set_text(ctx, st->entry, "");
	say(ctx, st, "It looks like you're writing an RTOS. Need help?");
	show_answer(ctx, st,
		    "Ask me about Zephyr and press Enter.\n\n"
		    "Answers come from a retrieval index built over the Zephyr\n"
		    "documentation, so they cite the page they came from rather\n"
		    "than inventing one.\n\n"
		    "The model runs on CPU and takes tens of seconds. Nothing is\n"
		    "blocked while it thinks -- drag this window about, or go and\n"
		    "use something else, and the answer will arrive here.");

	/* The animation clock. 0.7 added timers for Minesweeper; this is the
	 * other thing they were for -- something to look at while a slow
	 * request is outstanding.
	 */
	api->timer_start(ctx, TICK_MS, TIMER_ID);

	return 0;
}

static void clippy_event(zd_zapp_ctx_t ctx, const struct zd_event *ev)
{
	struct state *st = state_of(ctx);

	if (st == NULL) {
		return;
	}

	/* Type first, window second. Two of the events this zapp lives on --
	 * ZD_EV_TIMER and ZD_EV_HTTP_RESPONSE -- arrive with ev->win == NULL,
	 * so an `ev->win != st->win` guard at the top would drop both.
	 */
	switch (ev->type) {
	case ZD_EV_TIMER:
		if (ev->timer.id != TIMER_ID) {
			return;
		}
		st->tick++;
		/* Only the moods that move are redrawn. Repainting a still
		 * frame every 400 ms would be work the desktop does not need.
		 */
		if (st->mood == CLIPPY_IDLE || st->mood == CLIPPY_BUSY) {
			draw_face(ctx, st);
		}
		if (st->waiting) {
			say_waiting(ctx, st);
		}
		return;

	case ZD_EV_HTTP_RESPONSE:
		if (!st->waiting || ev->http.id != st->request) {
			/* Not ours, or arrived after we stopped caring. Still
			 * release it -- the slot is held until someone does.
			 */
			host->http_release(ctx, ev->http.id);
			return;
		}
		answered(ctx, st, ev);
		return;

	default:
		break;
	}

	if (ev->win != st->win) {
		return;
	}

	switch (ev->type) {
	case ZD_EV_KEY:
		if (ev->key.code == ZD_KEY_ENTER) {
			ask(ctx, st);
		}
		break;
	default:
		break;
	}
}

static void clippy_fini(zd_zapp_ctx_t ctx)
{
	struct state *st = state_of(ctx);

	host->timer_stop(ctx, TIMER_ID);

	/* An outstanding request is not cancelled here -- the desktop drops a
	 * response whose owner has gone, and clears the slot in its own pump.
	 * Releasing an id we may already have released would be the error.
	 */
	if (st != NULL) {
		st->used = false;
	}
	host->set_user_data(ctx, NULL);
}

struct zd_zapp_manifest zd_zapp_manifest = {
	.magic = ZD_ZAPP_MAGIC,
	.abi_major = ZD_ABI_MAJOR,
	.abi_minor = ZD_ABI_MINOR,
	.flags = 0,
	.name = "Clippy",
	.icon = NULL,
	.init = clippy_init,
	.event = clippy_event,
	.fini = clippy_fini,
};
LL_EXTENSION_SYMBOL(zd_zapp_manifest);
