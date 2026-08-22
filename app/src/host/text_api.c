/*
 * zephyr-desktop — implementation of the text widget.
 *
 * Three things here are worth knowing before reading the rest.
 *
 * ONE. The selection range is read through lv_textarea_get_label() and
 * lv_label_get_text_selection_start()/_end(), all public. The textarea itself
 * exposes only "is anything selected", which looks like a dead end and is not:
 * the selection lives on the label underneath, where there are getters. No LVGL
 * private header is included anywhere in this project, and it stays that way.
 *
 * TWO. LVGL counts positions in CHARACTERS and this ABI hands out BYTES.
 * They agree for ASCII, which is all the keymap and the on-screen keyboard can
 * produce, but a file read off the card can hold UTF-8 and then they do not.
 * byte_of()/char_of() convert, and every boundary between the two is marked.
 *
 * THREE. Changes the zapp itself made do not raise ZD_EV_TEXT_CHANGED. The
 * event means "the user typed", which is what a dirty flag wants; and without
 * the suppression a zapp calling text_set_text() would be re-entered from
 * inside its own host call, which is legal here but is a trap nobody needs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "text_api.h"
#include "clipboard.h"
#include "../chrome/theme.h"
#include "../input/keys.h"
#include "../loader/zapp_instance.h"
#include "../shell/osk.h"
#include "../wm/handle.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

struct text_rec {
	lv_obj_t *obj;
	struct zd_client *client;
	struct zd_zapp_instance *owner;
	uintptr_t handle;
	uint32_t suppress; /**< depth of host-initiated edits; see THREE above */
	bool readonly;
	bool used;
};

static bool drop_selection(struct text_rec *rec);

static bool drop_selection_obj(lv_obj_t *ta);

static struct text_rec recs[CONFIG_ZD_MAX_TEXTS];
static uint32_t live_texts;

/*
 * One shared edit buffer.
 *
 * Deleting a selection is prefix + suffix, and doing it in place with LVGL's
 * one-character-at-a-time delete is quadratic -- select-all over a full
 * document would be megabytes of memmove. This is the scratch that makes it
 * linear. Static rather than stack because CONFIG_ZD_TEXT_MAX does not belong
 * on a 16 KB main stack underneath an LVGL dispatch, and safe because the
 * desktop is single-threaded and this never outlives one call.
 */
static char edit_buf[CONFIG_ZD_TEXT_MAX + 1];

/* --- byte and character positions -------------------------------------------- */

/** Byte offset of character @p index. Clamps at the terminator. */
static uint32_t byte_of(const char *s, uint32_t index)
{
	uint32_t b = 0;

	while (index-- > 0 && s[b] != '\0') {
		b++;
		while ((s[b] & 0xC0) == 0x80) { /* UTF-8 continuation */
			b++;
		}
	}

	return b;
}

/** Character index of byte offset @p at. */
static uint32_t char_of(const char *s, uint32_t at)
{
	uint32_t index = 0;

	for (uint32_t b = 0; b < at && s[b] != '\0'; b++) {
		if ((s[b] & 0xC0) != 0x80) {
			index++;
		}
	}

	return index;
}

/* --- records ----------------------------------------------------------------- */

static struct text_rec *rec_of(struct zd_zapp_instance *owner, uintptr_t handle)
{
	return zd_handle_deref(handle, ZD_HANDLE_TEXT, owner);
}

static const char *text_of(const struct text_rec *rec)
{
	const char *s = lv_textarea_get_text(rec->obj);

	return s != NULL ? s : "";
}

/*
 * Released by LVGL, not by us.
 *
 * A text widget can die two ways: the zapp destroys it, or the window it lives
 * in is reaped and lv_obj_delete() takes the whole subtree. Hanging the cleanup
 * on LV_EVENT_DELETE covers both with one path, so there is no way to close a
 * window and leak a handle -- which is the failure the handle registry exists
 * to make loud, and it would rather not have to.
 */
static void text_deleted(lv_event_t *e)
{
	struct text_rec *rec = lv_event_get_user_data(e);

	if (!rec->used) {
		return;
	}

	if (rec->client != NULL && rec->client->text_focus == rec->obj) {
		rec->client->text_focus = NULL;
	}

	zd_handle_free(rec->handle);
	rec->used = false;
	rec->obj = NULL;
	live_texts--;
}

static void text_changed(lv_event_t *e)
{
	struct text_rec *rec = lv_event_get_user_data(e);
	struct zd_event ev = {
		.type = ZD_EV_TEXT_CHANGED,
		.win = (zd_window_t)rec->client->handle,
		.text = { .text = (zd_text_t)rec->handle },
	};

	if (rec->suppress > 0 || rec->owner == NULL || rec->client->handle == 0) {
		return;
	}

	zd_zapp_dispatch(rec->owner, &ev);
}

/* Clicking a text widget gives it the caret within its window. */
static void text_clicked(lv_event_t *e)
{
	struct text_rec *rec = lv_event_get_user_data(e);

	if (!rec->readonly) {
		zd_text_focus(rec->client, rec->obj);
	}
}

/* --- creation ---------------------------------------------------------------- */

static struct text_rec *claim_rec(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(recs); i++) {
		if (!recs[i].used) {
			return &recs[i];
		}
	}

	return NULL;
}

uintptr_t zd_text_create(struct zd_zapp_instance *owner, struct zd_client *client,
			 const struct zd_rect *geom, uint32_t flags)
{
	struct text_rec *rec = claim_rec();
	lv_obj_t *ta;

	if (rec == NULL) {
		LOG_WRN("text widget table full (%d)", CONFIG_ZD_MAX_TEXTS);
		return 0;
	}

	ta = lv_textarea_create(client->content);
	lv_obj_remove_style_all(ta);
	lv_obj_set_pos(ta, geom->x, geom->y);
	lv_obj_set_size(ta, geom->w, geom->h);

	lv_textarea_set_max_length(ta, CONFIG_ZD_TEXT_MAX);
	lv_textarea_set_one_line(ta, (flags & ZD_TEXT_ONE_LINE) != 0);
	lv_textarea_set_text_selection(ta, true);
	lv_textarea_set_cursor_click_pos(ta, true);
	lv_textarea_set_text(ta, "");

	lv_obj_set_style_bg_color(ta, lv_color_hex(ZD_C_LIGHT), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_text_color(ta, lv_color_hex(ZD_C_TEXT), LV_PART_MAIN);
	lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, LV_PART_MAIN);
	lv_obj_set_style_pad_all(ta, 2, LV_PART_MAIN);

	/* lv_obj_remove_style_all() takes the caret's colour with it, so the
	 * cursor part has to be dressed explicitly or it draws nothing.
	 * Transparent until this widget holds the caret -- every textarea
	 * blinks from construction otherwise, and two blinking carets in one
	 * window is a lie about where typing will go.
	 */
	lv_obj_set_style_bg_color(ta, lv_color_hex(ZD_C_TEXT), LV_PART_CURSOR);
	lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
	lv_obj_set_style_width(ta, 1, LV_PART_CURSOR);

	/* Bubble so a press anywhere in the widget still raises and focuses the
	 * window, exactly as it does for a label or the content area.
	 */
	lv_obj_add_flag(ta, LV_OBJ_FLAG_EVENT_BUBBLE);

	rec->obj = ta;
	rec->client = client;
	rec->owner = owner;
	rec->suppress = 0;
	rec->readonly = (flags & ZD_TEXT_READONLY) != 0;
	rec->used = true;

	rec->handle = zd_handle_alloc(ZD_HANDLE_TEXT, rec, owner);
	if (rec->handle == 0) {
		rec->used = false;
		lv_obj_delete(ta);
		return 0;
	}

	live_texts++;

	lv_obj_add_event_cb(ta, text_deleted, LV_EVENT_DELETE, rec);
	lv_obj_add_event_cb(ta, text_changed, LV_EVENT_VALUE_CHANGED, rec);
	lv_obj_add_event_cb(ta, text_clicked, LV_EVENT_PRESSED, rec);

	/* The first editable widget in a window takes the caret, so a zapp with
	 * one text area needs no focus call at all.
	 */
	if (!rec->readonly && client->text_focus == NULL) {
		zd_text_focus(client, ta);
	}

	return rec->handle;
}

void zd_text_focus(struct zd_client *client, lv_obj_t *ta)
{
	if (client->text_focus == ta) {
		return;
	}

	if (client->text_focus != NULL) {
		lv_obj_set_style_bg_opa(client->text_focus, LV_OPA_TRANSP, LV_PART_CURSOR);
	}

	client->text_focus = ta;

	if (ta != NULL) {
		lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
		/* On a board with no keys, the thing you just tapped into has
		 * to be typeable without a separate trip to the taskbar.
		 */
		zd_osk_wanted(true);
	}
}

void zd_text_destroy(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct text_rec *rec = rec_of(owner, handle);

	if (rec != NULL) {
		/* The DELETE handler does the bookkeeping, so this is the same
		 * path a window close takes.
		 */
		lv_obj_delete(rec->obj);
	}
}

/* --- content ----------------------------------------------------------------- */

int zd_text_set_text(struct zd_zapp_instance *owner, uintptr_t handle, const char *s)
{
	struct text_rec *rec = rec_of(owner, handle);

	if (rec == NULL || s == NULL) {
		return -EINVAL;
	}

	rec->suppress++;
	lv_textarea_set_text(rec->obj, s);

	/* Show the start of what was just set.
	 *
	 * lv_textarea_set_text() puts the caret at the end, because that is
	 * where an insertion point belongs after typing -- and the view follows
	 * the caret, so replacing the whole contents leaves the reader looking
	 * at the last line. That is never what "here is the new text" meant. A
	 * zapp loading a file wants the top of the file; one showing an answer
	 * wants the first sentence of it.
	 *
	 * Note the scroll as well as the caret. Moving the caret alone is not
	 * enough on a read-only widget, whose cursor is hidden and so does not
	 * drag the view along behind it. clippy showed the middle of its own
	 * greeting for exactly that reason.
	 */
	lv_textarea_set_cursor_pos(rec->obj, 0);
	lv_obj_scroll_to_y(rec->obj, 0, LV_ANIM_OFF);
	rec->suppress--;

	return 0;
}

int zd_text_get_text(struct zd_zapp_instance *owner, uintptr_t handle, uint32_t from,
		     char *buf, uint32_t len)
{
	struct text_rec *rec = rec_of(owner, handle);
	const char *s;
	uint32_t total;
	uint32_t take;

	if (rec == NULL || buf == NULL || len == 0) {
		return -EINVAL;
	}

	s = text_of(rec);
	total = (uint32_t)strlen(s);

	if (from >= total) {
		buf[0] = '\0';
		return 0;
	}

	take = MIN(total - from, MIN(len - 1, (uint32_t)CONFIG_ZD_FS_IO_CHUNK));
	memcpy(buf, s + from, take);
	buf[take] = '\0';

	return (int)take;
}

int zd_text_get_length(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct text_rec *rec = rec_of(owner, handle);

	return rec != NULL ? (int)strlen(text_of(rec)) : -EINVAL;
}

int zd_text_get_capacity(struct zd_zapp_instance *owner, uintptr_t handle)
{
	return rec_of(owner, handle) != NULL ? CONFIG_ZD_TEXT_MAX : -EINVAL;
}

int zd_text_insert(struct zd_zapp_instance *owner, uintptr_t handle, const char *s)
{
	struct text_rec *rec = rec_of(owner, handle);

	if (rec == NULL || s == NULL) {
		return -EINVAL;
	}

	rec->suppress++;
	drop_selection(rec);
	lv_textarea_add_text(rec->obj, s);
	rec->suppress--;

	return 0;
}

int zd_text_set_geometry(struct zd_zapp_instance *owner, uintptr_t handle,
			 const struct zd_rect *geom)
{
	struct text_rec *rec = rec_of(owner, handle);

	if (rec == NULL || geom == NULL) {
		return -EINVAL;
	}

	lv_obj_set_pos(rec->obj, geom->x, geom->y);
	lv_obj_set_size(rec->obj, geom->w, geom->h);
	return 0;
}

/* --- cursor and selection ----------------------------------------------------- */

int zd_text_set_cursor(struct zd_zapp_instance *owner, uintptr_t handle, uint32_t pos)
{
	struct text_rec *rec = rec_of(owner, handle);

	if (rec == NULL) {
		return -EINVAL;
	}

	lv_textarea_set_cursor_pos(rec->obj, (int32_t)char_of(text_of(rec), pos));
	return 0;
}

int zd_text_get_cursor(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct text_rec *rec = rec_of(owner, handle);

	if (rec == NULL) {
		return -EINVAL;
	}

	return (int)byte_of(text_of(rec), lv_textarea_get_cursor_pos(rec->obj));
}

/** Selected range in characters. @return false if nothing is selected. */
static bool selection_of(lv_obj_t *ta, uint32_t *from, uint32_t *to)
{
	lv_obj_t *label = lv_textarea_get_label(ta);
	uint32_t a = lv_label_get_text_selection_start(label);
	uint32_t b = lv_label_get_text_selection_end(label);

	if (a == LV_DRAW_LABEL_NO_TXT_SEL || b == LV_DRAW_LABEL_NO_TXT_SEL || a == b) {
		return false;
	}

	*from = MIN(a, b);
	*to = MAX(a, b);
	return true;
}

static bool selection_chars(const struct text_rec *rec, uint32_t *from, uint32_t *to)
{
	return selection_of(rec->obj, from, to);
}

int zd_text_get_selection(struct zd_zapp_instance *owner, uintptr_t handle, uint32_t *from,
			  uint32_t *to)
{
	struct text_rec *rec = rec_of(owner, handle);
	uint32_t a;
	uint32_t b;

	if (rec == NULL || from == NULL || to == NULL) {
		return -EINVAL;
	}

	if (!selection_chars(rec, &a, &b)) {
		*from = 0;
		*to = 0;
		return 0;
	}

	*from = byte_of(text_of(rec), a);
	*to = byte_of(text_of(rec), b);
	return 1;
}

int zd_text_select(struct zd_zapp_instance *owner, uintptr_t handle, uint32_t from,
		   uint32_t to)
{
	struct text_rec *rec = rec_of(owner, handle);
	lv_obj_t *label;
	const char *s;

	if (rec == NULL) {
		return -EINVAL;
	}

	label = lv_textarea_get_label(rec->obj);
	s = text_of(rec);

	if (from >= to) {
		lv_textarea_clear_selection(rec->obj);
		return 0;
	}

	lv_label_set_text_selection_start(label, char_of(s, from));
	lv_label_set_text_selection_end(label, char_of(s, to));
	lv_textarea_set_cursor_pos(rec->obj, (int32_t)char_of(s, to));
	return 0;
}

/*
 * Remove the selected run, if any, and leave the caret where it was.
 *
 * Rebuilt rather than deleted character by character: LVGL's delete_char moves
 * the whole tail each time, so clearing a select-all over a full document would
 * be O(n^2) and measurable on a 240 MHz part. This is one pass.
 */
static bool drop_selection_obj(lv_obj_t *ta)
{
	uint32_t from_char;
	uint32_t to_char;
	const char *s;
	uint32_t from;
	uint32_t to;
	uint32_t tail;

	if (!selection_of(ta, &from_char, &to_char)) {
		return false;
	}

	s = lv_textarea_get_text(ta);
	if (s == NULL) {
		return false;
	}
	from = byte_of(s, from_char);
	to = byte_of(s, to_char);
	tail = (uint32_t)strlen(s) - to;

	if (from + tail > CONFIG_ZD_TEXT_MAX) {
		return false; /* cannot happen; the widget is capped */
	}

	memcpy(edit_buf, s, from);
	memcpy(edit_buf + from, s + to, tail);
	edit_buf[from + tail] = '\0';

	lv_textarea_clear_selection(ta);
	lv_textarea_set_text(ta, edit_buf);
	lv_textarea_set_cursor_pos(ta, (int32_t)from_char);
	return true;
}

static bool drop_selection(struct text_rec *rec)
{
	return drop_selection_obj(rec->obj);
}

int zd_text_delete_selection(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct text_rec *rec = rec_of(owner, handle);
	bool dropped;

	if (rec == NULL) {
		return -EINVAL;
	}

	rec->suppress++;
	dropped = drop_selection(rec);
	rec->suppress--;

	return dropped ? 1 : 0;
}

uint32_t zd_text_live_count(void)
{
	return live_texts;
}

/* --- cut, copy, paste ---------------------------------------------------------- */

/*
 * The three verbs live here rather than in each zapp, and that is not a
 * convenience either. "Copy" is one line; "cut" is delete-then-store in the
 * right order, and "paste" is replace-the-selection-then-insert -- and the
 * empty-selection case of each is exactly where a reimplementation goes wrong.
 * Getting one desktop-wide answer means two zapps agree about what Ctrl+V does.
 */

int zd_text_copy(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct text_rec *rec = rec_of(owner, handle);
	uint32_t from_char;
	uint32_t to_char;
	const char *s;
	uint32_t from;
	uint32_t to;

	if (rec == NULL) {
		return -EINVAL;
	}

	if (!selection_chars(rec, &from_char, &to_char)) {
		return 0; /* nothing selected is not a failure, it is a no-op */
	}

	s = text_of(rec);
	from = byte_of(s, from_char);
	to = byte_of(s, to_char);

	return zd_clipboard_set(s + from, to - from);
}

int zd_text_cut(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct text_rec *rec = rec_of(owner, handle);
	int copied;

	if (rec == NULL) {
		return -EINVAL;
	}

	/* Copy before deleting, and only delete if the copy worked: a cut that
	 * removed the text and then failed to store it has destroyed it.
	 */
	copied = zd_text_copy(owner, handle);
	if (copied <= 0) {
		return copied;
	}

	rec->suppress++;
	drop_selection(rec);
	rec->suppress--;

	return copied;
}

int zd_text_paste(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct text_rec *rec = rec_of(owner, handle);
	uint32_t got = 0;
	int chunk;

	if (rec == NULL) {
		return -EINVAL;
	}

	if (zd_clipboard_length() == 0) {
		return 0;
	}

	rec->suppress++;
	drop_selection(rec);

	/* The clipboard read is short by contract, so loop -- the desktop obeys
	 * its own rules rather than reaching past them because it can.
	 */
	while ((chunk = zd_clipboard_get(got, edit_buf, sizeof(edit_buf))) > 0) {
		lv_textarea_add_text(rec->obj, edit_buf);
		got += (uint32_t)chunk;
	}

	rec->suppress--;

	return chunk < 0 ? chunk : (int)got;
}

/* --- keys --------------------------------------------------------------------- */

/** Byte offset of the start of the line the caret is on. */
static uint32_t line_start(const char *s, uint32_t at)
{
	while (at > 0 && s[at - 1] != '\n') {
		at--;
	}

	return at;
}

static uint32_t line_end(const char *s, uint32_t at)
{
	while (s[at] != '\0' && s[at] != '\n') {
		at++;
	}

	return at;
}

bool zd_text_key_obj(lv_obj_t *ta, uint32_t code, uint32_t unicode, uint16_t mods)
{
	const char *s;
	uint32_t at;

	if (ta == NULL) {
		return false;
	}

	switch (code) {
	case ZD_KEY_CHAR:
		/* Typing replaces a selection. LVGL's add_char does not do
		 * that on its own, and an editor where it does not is wrong in
		 * a way people notice immediately.
		 */
		drop_selection_obj(ta);
		lv_textarea_add_char(ta, unicode);
		return true;

	case ZD_KEY_ENTER:
		if (lv_textarea_get_one_line(ta)) {
			return false; /* a single-line field means it as "done" */
		}
		drop_selection_obj(ta);
		lv_textarea_add_char(ta, '\n');
		return true;

	case ZD_KEY_TAB:
		drop_selection_obj(ta);
		lv_textarea_add_text(ta, "    ");
		return true;

	case ZD_KEY_BACKSPACE:
		if (!drop_selection_obj(ta)) {
			lv_textarea_delete_char(ta);
		}
		return true;

	case ZD_KEY_DELETE:
		if (!drop_selection_obj(ta)) {
			lv_textarea_delete_char_forward(ta);
		}
		return true;

	case ZD_KEY_LEFT:
		lv_textarea_clear_selection(ta);
		lv_textarea_cursor_left(ta);
		return true;

	case ZD_KEY_RIGHT:
		lv_textarea_clear_selection(ta);
		lv_textarea_cursor_right(ta);
		return true;

	case ZD_KEY_UP:
		lv_textarea_clear_selection(ta);
		lv_textarea_cursor_up(ta);
		return true;

	case ZD_KEY_DOWN:
		lv_textarea_clear_selection(ta);
		lv_textarea_cursor_down(ta);
		return true;

	case ZD_KEY_HOME:
	case ZD_KEY_END:
		/* Line-scoped, the way an editor's are. LVGL only offers
		 * document start and end, which is what Ctrl+Home and Ctrl+End
		 * mean -- so both are here rather than one being borrowed for
		 * the other. ALT stands in for CTRL because CTRL never reaches
		 * a text widget at all; see wm/keys.c.
		 */
		lv_textarea_clear_selection(ta);
		s = lv_textarea_get_text(ta);
		s = s != NULL ? s : "";
		at = byte_of(s, lv_textarea_get_cursor_pos(ta));

		if (mods & ZD_MOD_ALT) {
			at = code == ZD_KEY_HOME ? 0 : (uint32_t)strlen(s);
		} else {
			at = code == ZD_KEY_HOME ? line_start(s, at) : line_end(s, at);
		}

		lv_textarea_set_cursor_pos(ta, (int32_t)char_of(s, at));
		return true;

	case ZD_KEY_PAGE_UP:
	case ZD_KEY_PAGE_DOWN:
		lv_textarea_clear_selection(ta);
		for (int i = 0; i < CONFIG_ZD_TEXT_PAGE_LINES; i++) {
			if (code == ZD_KEY_PAGE_UP) {
				lv_textarea_cursor_up(ta);
			} else {
				lv_textarea_cursor_down(ta);
			}
		}
		return true;

	default:
		/* Escape and the function keys mean nothing to a text field and
		 * everything to whoever owns it.
		 */
		return false;
	}
}

bool zd_text_on_client_key(struct zd_client *client, uint32_t code, uint32_t unicode,
			   uint16_t mods)
{
	return zd_text_key_obj(client->text_focus, code, unicode, mods);
}
