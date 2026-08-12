/*
 * zephyr-desktop — implementation of the two dialogs.
 *
 * One at a time, desktop-wide, on the overlay layer over a shade that eats
 * every press outside it. Rebuilt each time it opens, like the launcher menu
 * and the menu drop-down, because keeping a dialog per zapp alive to save a few
 * hundred microseconds of construction would be the wrong trade in both memory
 * and complexity.
 *
 * BOTH ENDS ARE DEFERRED, and the second one is less obvious than the first.
 *
 * Answering is deferred for the reason every deferral in this project exists: a
 * zapp told "yes, discard it" will very reasonably close its window, from a
 * stack frame standing inside the LVGL dispatch of the button it just clicked.
 *
 * OPENING is deferred because the answer to one dialog is very often another
 * dialog. Notepad's close handshake is exactly this: "save it?" -> Yes -> "save
 * as what?". Building the second dialog inline would lv_obj_clean() the panel
 * holding the Yes button whose dispatch is still on the stack -- the same
 * use-after-free, arrived at from the opposite direction. So a request is
 * recorded, the dialog is open as far as everyone else is concerned, and
 * zd_dialog_reap() builds it one loop iteration later.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dialog.h"
#include "osk.h"
#include "../chrome/rowlist.h"
#include "../chrome/theme.h"
#include "../chrome/titlebar.h"
#include "../host/bus_arb.h"
#include "../host/session.h"
#include "../host/text_api.h"
#include "../loader/zapp_instance.h"
#include "../wm/wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

#define TITLE_H  (16 + CONFIG_ZD_TOUCH_SLOP_PX)
#define BTN_H    (18 + CONFIG_ZD_TOUCH_SLOP_PX)
#define BTN_W    66
#define BTN_GAP  6
#define PAD      6
#define FIELD_H  (18 + CONFIG_ZD_TOUCH_SLOP_PX)
#define WHERE_H  14

/* Buttons sit in a row, so they are made wide rather than given hit area they
 * do not occupy -- BTN_W is already generous for a thumb. Same rule as
 * everywhere else; see wm/wm.h.
 */
BUILD_ASSERT(BTN_W >= 44, "a dialog button is too narrow to hit");

/*
 * A dialog that has been asked for but not yet built. See the header comment:
 * the request is recorded here and turned into widgets from the desktop loop.
 */
static struct {
	bool valid;
	bool is_file;
	char title[32];
	char msg[128];
	uint32_t arg; /**< buttons for a confirm, mode for a file dialog */
} want;

/*
 * Row ids in the picker's list.
 *
 * The id is documented as the caller's -- the desktop only hands it back -- and
 * here the caller is us, so it carries the one bit the click handler needs.
 * The alternative was a parallel array of types indexed by row, which is the
 * same information kept somewhere it can fall out of step.
 */
#define ROW_IS_DIR 0x8000u
#define ROW_UP     0x7FFFu /**< the ".." row; never a real entry's index */

static struct {
	lv_obj_t *shade;
	lv_obj_t *panel;
	struct zd_rowlist *rl; /**< file dialog only */
	lv_obj_t *where;       /**< file dialog only: which directory this is */
	lv_obj_t *field;       /**< file dialog, save mode only */

	struct zd_zapp_instance *owner;
	struct zd_client *client;
	const struct zd_session *session;
	struct zd_layers *layers;

	uint16_t id;
	bool open;
	bool dismissed;
	bool is_file;
	bool relist; /**< the directory changed; refill from the loop */
	char root[ZD_PATH_MAX]; /**< the directory the caller named; the floor */
	char dir[ZD_PATH_MAX];  /**< where we are now, at or below root */
	char path[ZD_PATH_MAX]; /**< the answer, valid after ZD_EV_DIALOG */
} dlg;

static bool set_path(const char *name);
static void fill_model(void);
static void show_where(void);

/* --- answering ----------------------------------------------------------------- */

static void finish(int16_t result)
{
	struct zd_zapp_instance *owner = dlg.owner;
	struct zd_event ev = {
		.type = ZD_EV_DIALOG,
		.win = (zd_window_t)(dlg.client != NULL ? dlg.client->handle : 0),
		.dialog = { .id = dlg.id, .result = result },
	};

	if (!dlg.open) {
		return;
	}

	LOG_DBG("dialog id %u answered %d", dlg.id, result);

	dlg.open = false;
	dlg.dismissed = true;
	dlg.owner = NULL;
	lv_obj_add_flag(dlg.panel, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(dlg.shade, LV_OBJ_FLAG_HIDDEN);

	/* Hidden first, dispatched second: whatever the zapp does next, the
	 * desktop must already believe the dialog is gone.
	 */
	if (owner != NULL) {
		zd_zapp_dispatch(owner, &ev);
	}
}

void zd_dialog_cancel(void)
{
	dlg.path[0] = '\0';
	finish(ZD_DLG_CANCEL);
}

static void build_confirm(void);
static void build_file(void);

void zd_dialog_reap(void)
{
	if (dlg.dismissed) {
		dlg.dismissed = false;
		dlg.rl = NULL;
		dlg.where = NULL;
		dlg.field = NULL;
		dlg.relist = false;
		lv_obj_clean(dlg.panel);
	}

	/* Empty first, then build: a request made while the previous dialog was
	 * still on screen must not be wiped by its cleanup.
	 */
	if (want.valid) {
		want.valid = false;
		LOG_DBG("dialog '%s' up (id %u, %s)", want.title, dlg.id,
			want.is_file ? "file" : "confirm");
		if (want.is_file) {
			build_file();
		} else {
			build_confirm();
		}
	}

	/* And a directory the user entered, which is the third deferral in this
	 * file and the same shape as the other two: the click that asked for it
	 * was dispatched from a row this refill replaces. Only the model is
	 * touched here; chrome/rowlist.c's own reap, which runs after this one,
	 * turns it back into widgets.
	 */
	if (dlg.relist) {
		dlg.relist = false;
		fill_model();
		show_where();
	}
}

void zd_dialog_owner_gone(struct zd_zapp_instance *inst)
{
	if (dlg.open && dlg.owner == inst) {
		/* No event: there is nobody left to tell. Just take it down, or
		 * the shade outlives the zapp and the desktop is unclickable.
		 */
		LOG_DBG("dialog dropped; its zapp is gone");
		dlg.owner = NULL;
		dlg.client = NULL;
		zd_dialog_cancel();
	}
}

bool zd_dialog_open(void)
{
	return dlg.open;
}

bool zd_dialog_open_for(const struct zd_zapp_instance *inst)
{
	return dlg.open && dlg.owner == inst;
}

/* --- widgets -------------------------------------------------------------------- */

static lv_obj_t *bare(lv_obj_t *parent)
{
	lv_obj_t *obj = lv_obj_create(parent);

	lv_obj_remove_style_all(obj);
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
	return obj;
}

static lv_obj_t *text_at(lv_obj_t *parent, const char *s, int32_t x, int32_t y,
			 int32_t w, uint32_t colour)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(label, s);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
	lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
	lv_obj_set_width(label, w);
	lv_obj_set_pos(label, x, y);
	return label;
}

static void button_clicked(lv_event_t *e)
{
	int16_t result = (int16_t)(intptr_t)lv_event_get_user_data(e);

	if (result == ZD_DLG_CANCEL) {
		zd_dialog_cancel();
		return;
	}

	/* In a file dialog, OK means "the name in the box", which the list and
	 * the field have been keeping up to date between them.
	 */
	if (dlg.is_file && dlg.field != NULL) {
		const char *name = lv_textarea_get_text(dlg.field);

		if (name == NULL || name[0] == '\0' || !set_path(name)) {
			return; /* nothing chosen, or it would not fit */
		}
	}

	if (dlg.is_file && dlg.path[0] == '\0') {
		return;
	}

	finish(result);
}

static lv_obj_t *add_button(int32_t x, int32_t y, const char *label, int16_t result)
{
	lv_obj_t *btn = bare(dlg.panel);
	lv_obj_t *text;

	lv_obj_add_style(btn, &zd_style_face, LV_PART_MAIN);
	lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(btn, BTN_W, BTN_H);
	lv_obj_set_pos(btn, x, y);
	zd_bevel_attach(btn, ZD_BEVEL_BUTTON);
	lv_obj_add_event_cb(btn, button_clicked, LV_EVENT_CLICKED,
			    (void *)(intptr_t)result);

	text = lv_label_create(btn);
	lv_label_set_text(text, label);
	lv_obj_set_style_text_font(text, &lv_font_montserrat_12, LV_PART_MAIN);
	lv_obj_set_style_text_color(text, lv_color_hex(ZD_C_TEXT), LV_PART_MAIN);
	lv_obj_center(text);

	return btn;
}

/** The panel, its titlebar, and the shade under it. @return the content top. */
static int32_t open_panel(const char *title, int32_t w, int32_t h)
{
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
	int32_t screen_h = lv_display_get_vertical_resolution(NULL);
	lv_obj_t *bar;

	lv_obj_clean(dlg.panel);
	dlg.dismissed = false;
	dlg.rl = NULL;
	dlg.where = NULL;
	dlg.field = NULL;
	dlg.relist = false;

	lv_obj_set_size(dlg.panel, w, h);
	lv_obj_set_pos(dlg.panel, (screen_w - w) / 2, (screen_h - h) / 3);

	bar = bare(dlg.panel);
	lv_obj_set_size(bar, w - 2 * ZD_FRAME_PAD, TITLE_H);
	lv_obj_set_pos(bar, ZD_FRAME_PAD, ZD_FRAME_PAD);
	zd_titlebar_set_active(bar, true);

	text_at(bar, title, 4, (TITLE_H - 12) / 2, w - 2 * ZD_FRAME_PAD - 8,
		ZD_C_TITLE_TEXT);

	lv_obj_remove_flag(dlg.shade, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(dlg.panel, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_to_index(dlg.shade, -1);
	lv_obj_move_to_index(dlg.panel, -1);

	return ZD_FRAME_PAD + TITLE_H + PAD;
}

/* --- confirm --------------------------------------------------------------------- */

/** Record what was asked for. The widgets happen in zd_dialog_reap(). */
static int request(struct zd_zapp_instance *owner, struct zd_client *client,
		   const char *title, uint16_t id, bool is_file)
{
	if (dlg.open) {
		return -EBUSY; /* one at a time, and system modal means system */
	}

	dlg.owner = owner;
	dlg.client = client;
	dlg.id = id;
	dlg.is_file = is_file;
	dlg.open = true;
	dlg.path[0] = '\0';

	want.valid = true;
	want.is_file = is_file;
	(void)strncpy(want.title, title != NULL ? title : "", sizeof(want.title) - 1);
	want.title[sizeof(want.title) - 1] = '\0';

	return 0;
}

int zd_dialog_confirm(struct zd_zapp_instance *owner, struct zd_client *client,
		      const char *title, const char *msg, uint32_t buttons, uint16_t id)
{
	int ret = request(owner, client, title, id, false);

	if (ret != 0) {
		return ret;
	}

	want.arg = buttons;
	(void)strncpy(want.msg, msg != NULL ? msg : "", sizeof(want.msg) - 1);
	want.msg[sizeof(want.msg) - 1] = '\0';

	return 0;
}

static void build_confirm(void)
{
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
	int32_t w = MIN(screen_w - 24, 272);
	int32_t body = 3 * 14; /* room for three wrapped lines of montserrat 12 */
	int32_t h = ZD_FRAME_PAD + TITLE_H + PAD + body + PAD + BTN_H + PAD;
	int32_t y;
	int32_t x;
	int n = (want.arg == ZD_DLG_YES_NO_CANCEL) ? 3 : 2;

	y = open_panel(want.title, w, h);
	text_at(dlg.panel, want.msg, PAD + ZD_FRAME_PAD, y,
		w - 2 * (PAD + ZD_FRAME_PAD), ZD_C_TEXT);

	/* Right-aligned, in the Win95 order: the affirmative first, Cancel
	 * last and nearest the corner.
	 */
	y = h - PAD - BTN_H;
	x = w - PAD - ZD_FRAME_PAD - n * BTN_W - (n - 1) * BTN_GAP;

	if (want.arg == ZD_DLG_YES_NO_CANCEL) {
		add_button(x, y, "Yes", ZD_DLG_YES);
		x += BTN_W + BTN_GAP;
		add_button(x, y, "No", ZD_DLG_NO);
		x += BTN_W + BTN_GAP;
	} else {
		add_button(x, y, "OK", ZD_DLG_OK);
		x += BTN_W + BTN_GAP;
	}

	add_button(x, y, "Cancel", ZD_DLG_CANCEL);
}

/* --- file picker ------------------------------------------------------------------ */

/*
 * Build dir/name into dlg.path, or refuse.
 *
 * Never truncates. A short path is not a smaller version of the right answer:
 * it still names a file, just the wrong one, and the caller would go on to
 * open it. The ABI promises dialog_get_path() gives back a whole path or
 * nothing, and this is where that promise is kept.
 */
static bool set_path(const char *name)
{
	int n = snprintf(dlg.path, sizeof(dlg.path), "%s/%s", dlg.dir, name);

	if (n < 0 || (size_t)n >= sizeof(dlg.path)) {
		LOG_WRN("'%s' does not fit in a %u-byte path", name,
			(unsigned int)sizeof(dlg.path));
		dlg.path[0] = '\0';
		return false;
	}

	return true;
}

/** Strip the last component of dlg.dir, unless that would leave the root. */
static bool go_up(void)
{
	char *slash;

	/* The floor is the directory the caller named, not the volume root.
	 * A picker opened on /tmp should not walk out of /tmp any more than one
	 * opened on the system directory should -- and this is also why there
	 * is no enum zd_dir member naming the parent of anything: the floor is
	 * a string we already have, and asking the session for one would make
	 * "up" mean something different depending on where you started.
	 */
	if (strcmp(dlg.dir, dlg.root) == 0) {
		return false;
	}

	slash = strrchr(dlg.dir, '/');
	if (slash == NULL || slash == dlg.dir) {
		return false;
	}

	*slash = '\0';
	return true;
}

/** Descend into @p name, or leave dlg.dir alone if the result will not fit. */
static bool go_down(const char *name)
{
	char next[ZD_PATH_MAX];
	int n = snprintf(next, sizeof(next), "%s/%s", dlg.dir, name);

	if (n < 0 || (size_t)n >= sizeof(next)) {
		LOG_WRN("'%s' is too deep for a %u-byte path", name,
			(unsigned int)sizeof(next));
		return false;
	}

	strcpy(dlg.dir, next);
	return true;
}

/*
 * Where we are, in as few characters as will still distinguish two places.
 *
 * The path itself does not fit: ZD_PATH_MAX is 192 and the panel is 272 pixels
 * wide. What fits, and is what the user actually wants to know, is the tail
 * starting at the directory they were shown first -- so a picker opened on the
 * home directory reads "user", then "user/docs", then "user/docs/old".
 */
static void show_where(void)
{
	const char *from = dlg.dir;
	const char *slash;

	if (dlg.where == NULL) {
		return;
	}

	slash = strrchr(dlg.root, '/');
	if (slash != NULL && slash > dlg.root) {
		from = dlg.dir + (slash - dlg.root) + 1;
	}

	lv_label_set_text(dlg.where, from);
}

static void entry_selected(void *user, int32_t index, uint16_t id)
{
	char name[ZD_NAME_MAX];

	ARG_UNUSED(user);

	if ((id & ROW_IS_DIR) != 0) {
		/* Selecting a directory is not choosing an answer. Blank the
		 * path so OK cannot quietly return the last file that was
		 * clicked before the user went wandering.
		 */
		dlg.path[0] = '\0';
		return;
	}

	if (zd_rowlist_item_text(dlg.rl, index, name, sizeof(name)) < 0) {
		return;
	}

	if (!set_path(name)) {
		return;
	}

	if (dlg.field != NULL) {
		lv_textarea_set_text(dlg.field, name);
	}
}

static void entry_activated(void *user, int32_t index, uint16_t id)
{
	char name[ZD_NAME_MAX];
	size_t len;

	ARG_UNUSED(user);

	if (id == ROW_UP) {
		dlg.relist = go_up();
		dlg.path[0] = '\0';
		return;
	}

	if (zd_rowlist_item_text(dlg.rl, index, name, sizeof(name)) < 0) {
		return;
	}

	if ((id & ROW_IS_DIR) != 0) {
		/* Directories are shown with a trailing slash, which no real
		 * filename can contain, so taking it back off is unambiguous.
		 */
		len = strlen(name);
		if (len > 0 && name[len - 1] == '/') {
			name[len - 1] = '\0';
		}

		dlg.path[0] = '\0';
		dlg.relist = go_down(name);
		return;
	}

	if (!set_path(name)) {
		return;
	}

	if (dlg.field != NULL) {
		/* Save mode: put the name in the box and stop. Finishing here
		 * would overwrite an existing file on a double-click, and there
		 * is no "are you sure?" behind it yet.
		 */
		lv_textarea_set_text(dlg.field, name);
		return;
	}

	finish(ZD_DLG_OK);
}

/*
 * Fill the model from the directory.
 *
 * Straight fs_readdir rather than the zapp shim: this is the desktop, the path
 * came from the session's own enum rather than from anything a zapp said, and
 * borrowing the caller's open-file quota to draw a picker would be wrong. The
 * bus arbiter still has to bracket it -- on the CoreS3 the card cannot answer
 * while the display owns GPIO35, and this is a filesystem call site like any
 * other. See docs/hardware.md.
 *
 * Nothing here touches LVGL, and that is a fix rather than a coincidence. The
 * version this replaces built rows inside the same bracket, which bus_arb.h
 * says never to do; splitting the model from the view made the two halves fall
 * into different functions on their own.
 *
 * Two passes so directories come first, which costs a second opendir and buys
 * a listing that reads the way every file manager's does.
 */
static void scan(bool dirs)
{
	struct fs_dir_t dir;
	char label[ZD_NAME_MAX];
	int ret;

	fs_dir_t_init(&dir);

	zd_bus_storage_acquire();
	ret = fs_opendir(&dir, dlg.dir);

	while (ret == 0) {
		static struct fs_dirent entry;
		bool is_dir;

		if (fs_readdir(&dir, &entry) != 0 || entry.name[0] == '\0') {
			break;
		}

		is_dir = entry.type != FS_DIR_ENTRY_FILE;
		if (is_dir != dirs) {
			continue;
		}

		if (is_dir) {
			(void)snprintf(label, sizeof(label), "%s/", entry.name);
		} else {
			(void)strncpy(label, entry.name, sizeof(label) - 1);
			label[sizeof(label) - 1] = '\0';
		}

		if (zd_rowlist_add(dlg.rl, label,
				   (uint16_t)(is_dir ? ROW_IS_DIR : 0)) < 0) {
			LOG_WRN("'%s' has more entries than the list will hold",
				dlg.dir);
			break;
		}
	}

	if (ret == 0) {
		fs_closedir(&dir);
	}
	zd_bus_storage_release();
}

static void fill_model(void)
{
	if (dlg.rl == NULL) {
		return;
	}

	zd_rowlist_clear(dlg.rl);

	if (strcmp(dlg.dir, dlg.root) != 0) {
		/* A display string, never a path: fs_shim.c rejects ".." as a
		 * component precisely so nobody can hand one to it, and go_up()
		 * truncates instead.
		 */
		(void)zd_rowlist_add(dlg.rl, "..", ROW_UP);
	}

	scan(true);
	scan(false);

	/* Permanent tracing. Navigation is the one part of a picker whose state
	 * is invisible from a screenshot -- two directories with similar
	 * contents look identical -- and this is the line that says which one is
	 * on screen. The menu trace added in K earned its keep the same way.
	 */
	LOG_DBG("picker listing %s (%d entries)", dlg.dir, zd_rowlist_count(dlg.rl));
}

int zd_dialog_file(struct zd_zapp_instance *owner, struct zd_client *client,
		   const char *title, enum zd_dir dir, uint32_t mode, uint16_t id)
{
	char probe[ZD_PATH_MAX];
	int ret;

	/* Resolve now rather than at build time: an unusable directory is the
	 * caller's error and should come back from the call that made it, not
	 * arrive silently a frame later.
	 */
	if (zd_session_path(dlg.session, dir, probe, sizeof(probe)) != 0) {
		return -EINVAL;
	}

	ret = request(owner, client, title, id, true);
	if (ret != 0) {
		return ret;
	}

	want.arg = mode;
	(void)strncpy(dlg.dir, probe, sizeof(dlg.dir) - 1);
	dlg.dir[sizeof(dlg.dir) - 1] = '\0';
	strcpy(dlg.root, dlg.dir);

	return 0;
}

static void build_file(void)
{
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
	int32_t screen_h = lv_display_get_vertical_resolution(NULL);
	int32_t w = MIN(screen_w - 24, 272);
	int32_t inner = w - 2 * (PAD + ZD_FRAME_PAD);
	bool save = (want.arg == ZD_DLG_SAVE);
	int32_t list_h;
	int32_t h;
	int32_t y;
	int32_t x;

	/* Six rows, or half the screen on a panel too short for six. The row
	 * height is the list's, not ours: it scales with the touch slop and
	 * getting a second copy of that arithmetic wrong is exactly how the
	 * chrome sizes drifted apart before.
	 */
	list_h = MIN(screen_h / 2, 6 * zd_rowlist_row_h());
	h = ZD_FRAME_PAD + TITLE_H + PAD + WHERE_H + list_h + PAD +
	    (save ? FIELD_H + PAD : 0) + BTN_H + PAD;

	y = open_panel(want.title, w, h);

	/* Which directory this is. Without it, navigation is a list that
	 * inexplicably changes -- the user has no other way to tell where "up"
	 * would go, or that there is anywhere to go up to.
	 */
	dlg.where = text_at(dlg.panel, "", PAD + ZD_FRAME_PAD, y, inner, ZD_C_TEXT);
	lv_label_set_long_mode(dlg.where, LV_LABEL_LONG_MODE_DOTS);
	y += WHERE_H;

	dlg.rl = zd_rowlist_create(dlg.panel, PAD + ZD_FRAME_PAD, y, inner, list_h);
	if (dlg.rl != NULL) {
		zd_rowlist_set_cb(dlg.rl, entry_selected, entry_activated, NULL);
		fill_model();
		show_where();
	}

	y += list_h + PAD;

	if (save) {
		dlg.field = lv_textarea_create(dlg.panel);
		lv_obj_remove_style_all(dlg.field);
		lv_obj_set_size(dlg.field, inner, FIELD_H);
		lv_obj_set_pos(dlg.field, PAD + ZD_FRAME_PAD, y);
		lv_textarea_set_one_line(dlg.field, true);
		lv_textarea_set_max_length(dlg.field, ZD_NAME_MAX - 1);
		lv_textarea_set_cursor_click_pos(dlg.field, true);
		lv_textarea_set_text(dlg.field, "");
		lv_obj_set_style_bg_color(dlg.field, lv_color_hex(ZD_C_LIGHT), LV_PART_MAIN);
		lv_obj_set_style_bg_opa(dlg.field, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_text_color(dlg.field, lv_color_hex(ZD_C_TEXT),
					    LV_PART_MAIN);
		lv_obj_set_style_text_font(dlg.field, &lv_font_montserrat_12, LV_PART_MAIN);
		lv_obj_set_style_pad_all(dlg.field, 2, LV_PART_MAIN);
		lv_obj_set_style_bg_color(dlg.field, lv_color_hex(ZD_C_TEXT),
					  LV_PART_CURSOR);
		lv_obj_set_style_bg_opa(dlg.field, LV_OPA_COVER, LV_PART_CURSOR);
		lv_obj_set_style_width(dlg.field, 1, LV_PART_CURSOR);
		zd_bevel_attach(dlg.field, ZD_BEVEL_IN);

		/* There is a field to type into and possibly no keyboard. */
		zd_osk_wanted(true);

		y += FIELD_H + PAD;
	}

	x = w - PAD - ZD_FRAME_PAD - 2 * BTN_W - BTN_GAP;
	add_button(x, y, save ? "Save" : "Open", ZD_DLG_OK);
	add_button(x + BTN_W + BTN_GAP, y, "Cancel", ZD_DLG_CANCEL);
}

int zd_dialog_get_path(struct zd_zapp_instance *owner, char *buf, uint32_t len)
{
	ARG_UNUSED(owner);

	if (buf == NULL || len == 0) {
		return -EINVAL;
	}

	if (strlen(dlg.path) >= len) {
		return -ENOSPC;
	}

	strcpy(buf, dlg.path);
	return (int)strlen(buf);
}

/* --- keys ------------------------------------------------------------------------- */

bool zd_dialog_key(uint32_t code, uint32_t unicode, uint16_t mods)
{
	if (!dlg.open) {
		return false;
	}

	if (code == ZD_KEY_ESCAPE) {
		zd_dialog_cancel();
		return true;
	}

	/* Arrows, Home, End and the page keys drive the list. Not Enter: what
	 * Enter means depends on which dialog this is, and that is decided
	 * below rather than by whichever widget answers first.
	 */
	if (dlg.rl != NULL && code != ZD_KEY_ENTER && zd_rowlist_key(dlg.rl, code, mods)) {
		return true;
	}

	if (code == ZD_KEY_ENTER) {
		/* Enter is the default button. In a save dialog that means
		 * "use the name in the box", which is why the field is
		 * single-line: zd_text_key_obj() declines Enter there rather
		 * than inserting a newline into a filename.
		 */
		if (dlg.is_file && dlg.field != NULL) {
			const char *name = lv_textarea_get_text(dlg.field);

			if (name != NULL && name[0] != '\0' && set_path(name)) {
				finish(ZD_DLG_OK);
			}
			return true;
		}

		/* An open dialog has no field, so Enter means "the row that is
		 * chosen" -- which for a directory means entering it, and is
		 * why this goes through the list rather than straight to OK.
		 */
		if (dlg.rl != NULL && zd_rowlist_key(dlg.rl, ZD_KEY_ENTER, mods)) {
			return true;
		}

		if (!dlg.is_file) {
			finish(ZD_DLG_OK);
		}
		return true;
	}

	if (dlg.field != NULL) {
		(void)zd_text_key_obj(dlg.field, code, unicode, mods);
	}

	/* Everything is swallowed either way. A dialog that let keys through to
	 * the window behind it would not be modal.
	 */
	return true;
}

/* --- boot -------------------------------------------------------------------------- */

static void shade_pressed(lv_event_t *e)
{
	ARG_UNUSED(e);

	/* Deliberately NOT a dismiss. Clicking outside a Win95 dialog did
	 * nothing but beep; a "save changes?" that answers itself because the
	 * user tapped the wrong place is how work gets lost.
	 */
}

void zd_dialog_init(struct zd_layers *layers, const struct zd_session *session)
{
	dlg.layers = layers;
	dlg.session = session;

	dlg.shade = lv_obj_create(layers->overlay);
	lv_obj_remove_style_all(dlg.shade);
	lv_obj_set_size(dlg.shade, LV_PCT(100), LV_PCT(100));
	lv_obj_set_pos(dlg.shade, 0, 0);
	lv_obj_add_flag(dlg.shade, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(dlg.shade, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(dlg.shade, shade_pressed, LV_EVENT_PRESSED, NULL);

	dlg.panel = lv_obj_create(layers->overlay);
	lv_obj_remove_style_all(dlg.panel);
	lv_obj_add_style(dlg.panel, &zd_style_face, LV_PART_MAIN);
	lv_obj_remove_flag(dlg.panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(dlg.panel, LV_OBJ_FLAG_HIDDEN);
	zd_bevel_attach(dlg.panel, ZD_BEVEL_OUT);
}
