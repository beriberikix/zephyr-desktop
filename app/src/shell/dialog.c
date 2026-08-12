/*
 * zephyr-desktop — implementation of the two dialogs.
 *
 * One at a time, desktop-wide, on the overlay layer over a shade that eats
 * every press outside it. Rebuilt each time it opens, like the launcher menu
 * and the menu drop-down, because keeping a dialog per zapp alive to save a few
 * hundred microseconds of construction would be the wrong trade in both memory
 * and complexity.
 *
 * Answering is DEFERRED, for the third time in this project and the same reason
 * every time: a zapp told "yes, discard it" will very reasonably close its
 * window, from a stack frame standing inside the LVGL dispatch of the button it
 * just clicked. The dialog is hidden immediately and zd_dialog_reap() empties
 * it from the desktop loop.
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
#define ROW_H    (16 + CONFIG_ZD_TOUCH_SLOP_PX)
#define PAD      6
#define FIELD_H  (18 + CONFIG_ZD_TOUCH_SLOP_PX)

/* Buttons sit in a row, so they are made wide rather than given hit area they
 * do not occupy -- BTN_W is already generous for a thumb. Same rule as
 * everywhere else; see wm/wm.h.
 */
BUILD_ASSERT(BTN_W >= 44, "a dialog button is too narrow to hit");

static struct {
	lv_obj_t *shade;
	lv_obj_t *panel;
	lv_obj_t *list;  /**< file dialog only */
	lv_obj_t *field; /**< file dialog, save mode only */

	struct zd_zapp_instance *owner;
	struct zd_client *client;
	const struct zd_session *session;
	struct zd_layers *layers;

	uint16_t id;
	bool open;
	bool dismissed;
	bool is_file;
	char dir[ZD_PATH_MAX];
	char path[ZD_PATH_MAX]; /**< the answer, valid after ZD_EV_DIALOG */
} dlg;

static bool set_path(const char *name);

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

void zd_dialog_reap(void)
{
	if (!dlg.dismissed) {
		return;
	}

	dlg.dismissed = false;
	dlg.list = NULL;
	dlg.field = NULL;
	lv_obj_clean(dlg.panel);
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
	dlg.list = NULL;
	dlg.field = NULL;

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

int zd_dialog_confirm(struct zd_zapp_instance *owner, struct zd_client *client,
		      const char *title, const char *msg, uint32_t buttons, uint16_t id)
{
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
	int32_t w = MIN(screen_w - 24, 272);
	int32_t body = 3 * 14; /* room for three wrapped lines of montserrat 12 */
	int32_t h = ZD_FRAME_PAD + TITLE_H + PAD + body + PAD + BTN_H + PAD;
	int32_t y;
	int32_t x;
	int n = (buttons == ZD_DLG_YES_NO_CANCEL) ? 3 : 2;

	if (dlg.open) {
		return -EBUSY; /* one at a time, and system modal means system */
	}

	dlg.owner = owner;
	dlg.client = client;
	dlg.id = id;
	dlg.is_file = false;
	dlg.open = true;
	dlg.path[0] = '\0';

	y = open_panel(title != NULL ? title : "", w, h);
	text_at(dlg.panel, msg != NULL ? msg : "", PAD + ZD_FRAME_PAD, y,
		w - 2 * (PAD + ZD_FRAME_PAD), ZD_C_TEXT);

	/* Right-aligned, in the Win95 order: the affirmative first, Cancel
	 * last and nearest the corner.
	 */
	y = h - PAD - BTN_H;
	x = w - PAD - ZD_FRAME_PAD - n * BTN_W - (n - 1) * BTN_GAP;

	if (buttons == ZD_DLG_YES_NO_CANCEL) {
		add_button(x, y, "Yes", ZD_DLG_YES);
		x += BTN_W + BTN_GAP;
		add_button(x, y, "No", ZD_DLG_NO);
		x += BTN_W + BTN_GAP;
	} else {
		add_button(x, y, "OK", ZD_DLG_OK);
		x += BTN_W + BTN_GAP;
	}

	add_button(x, y, "Cancel", ZD_DLG_CANCEL);

	return 0;
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

static void entry_clicked(lv_event_t *e)
{
	const char *name = lv_event_get_user_data(e);

	if (!set_path(name)) {
		return;
	}

	if (dlg.field != NULL) {
		lv_textarea_set_text(dlg.field, name);
	}
}

/*
 * List the directory.
 *
 * Straight fs_readdir rather than the zapp shim: this is the desktop, the path
 * came from the session's own enum rather than from anything a zapp said, and
 * borrowing the caller's open-file quota to draw a picker would be wrong. The
 * bus arbiter still has to bracket it -- on the CoreS3 the card cannot answer
 * while the display owns GPIO35, and this is a filesystem call site like any
 * other. See docs/hardware.md.
 */
static int fill_list(int32_t w)
{
	struct fs_dir_t dir;
	int32_t y = 0;
	int shown = 0;
	int ret;

	fs_dir_t_init(&dir);

	zd_bus_storage_acquire();
	ret = fs_opendir(&dir, dlg.dir);

	while (ret == 0 && shown < CONFIG_ZD_DIALOG_LIST_MAX) {
		static struct fs_dirent entry;
		lv_obj_t *row;
		lv_obj_t *label;
		char *name;

		if (fs_readdir(&dir, &entry) != 0 || entry.name[0] == '\0') {
			break;
		}

		if (entry.type != FS_DIR_ENTRY_FILE) {
			continue; /* no navigation yet; see the header comment */
		}

		row = bare(dlg.list);
		lv_obj_add_style(row, &zd_style_face, LV_PART_MAIN);
		lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(row, w, ROW_H);
		lv_obj_set_pos(row, 0, y);
		y += ROW_H;
		shown++;

		label = lv_label_create(row);
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
		lv_label_set_text(label, entry.name);
		lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
		lv_obj_set_style_text_color(label, lv_color_hex(ZD_C_TEXT), LV_PART_MAIN);
		lv_obj_set_width(label, w - 8);
		lv_obj_set_pos(label, 4, (ROW_H - 12) / 2);

		/* The label owns the only copy of the name that outlives this
		 * loop, so the row's callback reads it back out rather than
		 * keeping a pointer into `entry`, which is about to be reused.
		 */
		name = (char *)lv_label_get_text(label);
		lv_obj_add_event_cb(row, entry_clicked, LV_EVENT_CLICKED, name);
	}

	if (ret == 0) {
		fs_closedir(&dir);
	}
	zd_bus_storage_release();

	return shown;
}

int zd_dialog_file(struct zd_zapp_instance *owner, struct zd_client *client,
		   const char *title, enum zd_dir dir, uint32_t mode, uint16_t id)
{
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
	int32_t screen_h = lv_display_get_vertical_resolution(NULL);
	int32_t w = MIN(screen_w - 24, 272);
	int32_t inner = w - 2 * (PAD + ZD_FRAME_PAD);
	bool save = (mode == ZD_DLG_SAVE);
	int32_t list_h;
	int32_t h;
	int32_t y;
	int32_t x;

	if (dlg.open) {
		return -EBUSY;
	}

	if (zd_session_path(dlg.session, dir, dlg.dir, sizeof(dlg.dir)) != 0) {
		return -EINVAL;
	}

	list_h = MIN(screen_h / 2, 6 * ROW_H);
	h = ZD_FRAME_PAD + TITLE_H + PAD + list_h + PAD + (save ? FIELD_H + PAD : 0) +
	    BTN_H + PAD;

	dlg.owner = owner;
	dlg.client = client;
	dlg.id = id;
	dlg.is_file = true;
	dlg.open = true;
	dlg.path[0] = '\0';

	y = open_panel(title != NULL ? title : "", w, h);

	dlg.list = lv_obj_create(dlg.panel);
	lv_obj_remove_style_all(dlg.list);
	lv_obj_add_style(dlg.list, &zd_style_face, LV_PART_MAIN);
	lv_obj_set_style_bg_color(dlg.list, lv_color_hex(ZD_C_LIGHT), LV_PART_MAIN);
	lv_obj_set_size(dlg.list, inner, list_h);
	lv_obj_set_pos(dlg.list, PAD + ZD_FRAME_PAD, y);
	lv_obj_set_scroll_dir(dlg.list, LV_DIR_VER);
	zd_bevel_attach(dlg.list, ZD_BEVEL_IN);

	if (fill_list(inner) == 0) {
		text_at(dlg.list, "(no files)", 4, 4, inner - 8, ZD_C_SHADOW);
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

	return 0;
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
