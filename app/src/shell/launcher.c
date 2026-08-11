/*
 * zephyr-desktop — the Start menu popup.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "launcher.h"
#include "taskbar.h"
#include "../chrome/theme.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

#define MENU_W        140
#define ITEM_H        18
#define MENU_PAD      4
#define EMPTY_ITEM_H  20

static struct {
	lv_obj_t *panel;
	const struct zd_session *session;
	zd_launch_cb_t cb;
	void *cb_arg;
	struct zd_zapp_entry entries[ZD_MAX_DISCOVERED];
	int count;
	bool open;
} menu;

static void item_clicked(lv_event_t *e)
{
	int index = (int)(intptr_t)lv_event_get_user_data(e);

	zd_launcher_hide();

	if (index < 0 || index >= menu.count) {
		return;
	}

	LOG_INF("launch '%s' (%s)", menu.entries[index].name, menu.entries[index].path);

	if (menu.cb != NULL) {
		menu.cb(&menu.entries[index], menu.cb_arg);
	}
}

static lv_obj_t *add_item(int index, const char *text, bool enabled)
{
	lv_obj_t *item = lv_obj_create(menu.panel);
	lv_obj_t *label;

	lv_obj_remove_style_all(item);
	lv_obj_add_style(item, &zd_style_face, LV_PART_MAIN);
	lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_size(item, MENU_W - 2 * MENU_PAD, ITEM_H);
	lv_obj_set_pos(item, MENU_PAD, MENU_PAD + index * ITEM_H);

	label = lv_label_create(item);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
	lv_obj_set_style_text_color(label,
				    lv_color_hex(enabled ? ZD_C_TEXT : ZD_C_SHADOW),
				    LV_PART_MAIN);
	lv_obj_set_pos(label, 4, (ITEM_H - 12) / 2);

	if (enabled) {
		lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(item, CONFIG_ZD_TOUCH_SLOP_PX);
		lv_obj_add_event_cb(item, item_clicked, LV_EVENT_CLICKED,
				    (void *)(intptr_t)index);
	}

	return item;
}

void zd_launcher_refresh(void)
{
	lv_area_t launcher;
	int32_t height;
	int rows;

	lv_obj_clean(menu.panel);

	menu.count = zd_zapps_discover(menu.session, menu.entries, ZD_MAX_DISCOVERED);
	if (menu.count < 0) {
		menu.count = 0;
	}

	rows = menu.count > 0 ? menu.count : 1;
	height = 2 * MENU_PAD + rows * ITEM_H;

	lv_obj_set_size(menu.panel, MENU_W, height);

	/* Anchor to the launcher button's left edge, sitting on the taskbar. */
	zd_taskbar_launcher_coords(&launcher);
	lv_obj_set_pos(menu.panel, launcher.x1,
		       lv_display_get_vertical_resolution(NULL) - ZD_TASKBAR_H - height);

	if (menu.count == 0) {
		add_item(0, "(no apps found)", false);
		return;
	}

	for (int i = 0; i < menu.count; i++) {
		add_item(i, menu.entries[i].name, true);
	}
}

void zd_launcher_hide(void)
{
	if (!menu.open) {
		return;
	}
	menu.open = false;
	lv_obj_add_flag(menu.panel, LV_OBJ_FLAG_HIDDEN);
}

void zd_launcher_toggle(void)
{
	if (menu.open) {
		zd_launcher_hide();
		return;
	}

	/* Rescan on every open rather than caching: the whole point of the
	 * filesystem being the source of truth is that dropping a file in
	 * changes what the menu shows.
	 */
	zd_launcher_refresh();
	menu.open = true;
	lv_obj_remove_flag(menu.panel, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_to_index(menu.panel, -1);
}

void zd_launcher_init(struct zd_layers *layers, const struct zd_session *session,
		      zd_launch_cb_t cb, void *cb_arg)
{
	menu.session = session;
	menu.cb = cb;
	menu.cb_arg = cb_arg;

	/* Lives on the panel layer's parent -- the screen -- rather than inside
	 * the taskbar, because it has to overhang upward past the taskbar's own
	 * bounds while still drawing above every window.
	 */
	menu.panel = lv_obj_create(lv_obj_get_parent(layers->panel));
	lv_obj_remove_style_all(menu.panel);
	lv_obj_add_style(menu.panel, &zd_style_face, LV_PART_MAIN);
	lv_obj_remove_flag(menu.panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(menu.panel, LV_OBJ_FLAG_HIDDEN);
	zd_bevel_attach(menu.panel, ZD_BEVEL_OUT);

	zd_launcher_refresh();
}
