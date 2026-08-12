/*
 * zephyr-desktop — implementation of menu bars and drop-downs.
 *
 * Two structural decisions worth knowing.
 *
 * ONE. There is exactly one drop-down surface, desktop-wide, living on the
 * overlay layer with a click-swallowing shade beneath it. Only one menu can be
 * open at a time, which is true of every desktop anyone has used, and it means
 * the popup is built when it opens rather than kept alive for every menu in
 * every window.
 *
 * TWO. Dismissing the popup is DEFERRED. Choosing an item fires ZD_EV_MENU, and
 * the obvious thing for a zapp to do with File -> Exit is close its window --
 * from a callback standing on the row that was clicked. Deleting the row there
 * is a use-after-free, so the popup is only hidden, and zd_menu_reap() deletes
 * it from the desktop loop. This is the rule in CLAUDE.md, met from a third
 * direction after zapps and the taskbar.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zd/zapp_abi.h>

#include "menu.h"
#include "theme.h"
#include "../loader/zapp_instance.h"
#include "../wm/client.h"
#include "../wm/handle.h"
#include "../wm/wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/*
 * Rows grow with the touch slop; they never get hit-area slop. Third time:
 * see ITEM_H in shell/launcher.c and ZD_BTN_SZ in wm/wm.h. A contiguous list
 * with ext_click_area on every entry hands every tap to whichever was added
 * last, and the top entry becomes unreachable.
 */
#define ITEM_H     (16 + CONFIG_ZD_TOUCH_SLOP_PX)
#define TITLE_PAD  (6 + CONFIG_ZD_TOUCH_SLOP_PX / 2)
#define ITEM_PAD   8
#define SEP_H      5
#define POPUP_PAD  3
#define POPUP_MIN_W 76

#define LABEL_MAX 20

struct menu_item {
	char label[LABEL_MAX];
	uint16_t id;
	bool separator;
	bool enabled;
};

struct menu {
	bool used;
	bool is_bar;
	struct zd_zapp_instance *owner;
	struct zd_client *client;
	uintptr_t handle;

	/* bar */
	struct menu *subs[CONFIG_ZD_MENU_MAX_SUBMENUS];
	uint8_t sub_count;

	/* submenu */
	struct menu *bar;
	lv_obj_t *title;  /**< this submenu's button in the bar */
	struct menu_item items[CONFIG_ZD_MENU_MAX_ITEMS];
	uint8_t item_count;
};

static struct menu menus[CONFIG_ZD_MAX_MENUS];
static uint32_t live_menus;

static struct {
	lv_obj_t *shade; /**< full-screen, clickable, invisible: eats the miss */
	lv_obj_t *panel;
	struct menu *open;
	bool dismissed; /**< hidden, waiting for the reap to empty it */
} popup;

/* --- records ------------------------------------------------------------------ */

static struct menu *claim(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(menus); i++) {
		if (!menus[i].used) {
			memset(&menus[i], 0, sizeof(menus[i]));
			return &menus[i];
		}
	}

	return NULL;
}

static struct menu *menu_of(struct zd_zapp_instance *owner, uintptr_t handle)
{
	return zd_handle_deref(handle, ZD_HANDLE_MENU, owner);
}

static void release(struct menu *m)
{
	if (!m->used) {
		return;
	}

	m->used = false;
	zd_handle_free(m->handle);
	live_menus--;
}

/*
 * The bar and every drop-down hanging off it die with the window.
 *
 * Hung on LVGL's DELETE for the bar object, so there is one path whether the
 * zapp went away, the user hit the close box, or the desktop reaped a window
 * whose zapp had already gone -- the same reasoning as the text widget.
 */
static void bar_deleted(lv_event_t *e)
{
	struct menu *bar = lv_event_get_user_data(e);

	if (popup.open != NULL && (popup.open == bar || popup.open->bar == bar)) {
		zd_menu_close();
	}

	for (uint8_t i = 0; i < bar->sub_count; i++) {
		release(bar->subs[i]);
	}

	if (bar->client != NULL) {
		bar->client->menubar = NULL;
		bar->client->menubar_h = 0;
	}

	release(bar);
}

/* --- the drop-down ------------------------------------------------------------- */

void zd_menu_close(void)
{
	if (popup.open == NULL) {
		return;
	}

	popup.open = NULL;
	popup.dismissed = true;
	lv_obj_add_flag(popup.panel, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(popup.shade, LV_OBJ_FLAG_HIDDEN);
}

void zd_menu_reap(void)
{
	if (!popup.dismissed) {
		return;
	}

	popup.dismissed = false;
	lv_obj_clean(popup.panel);
}

static void item_clicked(lv_event_t *e)
{
	uintptr_t packed = (uintptr_t)lv_event_get_user_data(e);
	struct menu *m = popup.open;
	uint8_t index = (uint8_t)packed;
	struct zd_event ev;

	if (m == NULL || index >= m->item_count || !m->items[index].enabled) {
		return;
	}

	ev.type = ZD_EV_MENU;
	ev.win = (zd_window_t)m->client->handle;
	ev.menu.id = m->items[index].id;

	LOG_DBG("menu item '%s' chosen (id %u)", m->items[index].label, ev.menu.id);

	/* Hide first, dispatch second. The zapp may answer File -> Exit by
	 * closing its window, and the desktop must already believe the menu is
	 * gone by the time that happens.
	 */
	zd_menu_close();

	if (m->owner != NULL && m->client->handle != 0) {
		zd_zapp_dispatch(m->owner, &ev);
	}
}

static void shade_clicked(lv_event_t *e)
{
	ARG_UNUSED(e);
	zd_menu_close();
}

/** Width a label needs, measured rather than guessed. */
static int32_t label_width(lv_obj_t *parent, const char *text)
{
	lv_obj_t *probe = lv_label_create(parent);
	int32_t w;

	lv_obj_set_style_text_font(probe, &lv_font_montserrat_12, LV_PART_MAIN);
	lv_label_set_text(probe, text);
	/* A label sizes itself to its content, but not until a layout pass has
	 * run -- reading the width straight after set_text returns the OLD one.
	 * The same trap as lv_obj_set_size(); see milestone J.
	 */
	lv_obj_update_layout(probe);
	w = lv_obj_get_width(probe);
	lv_obj_delete(probe);

	return w;
}

static void build_popup(struct menu *m)
{
	int32_t screen_w = lv_display_get_horizontal_resolution(NULL);
	int32_t screen_h = lv_display_get_vertical_resolution(NULL);
	lv_area_t anchor;
	int32_t width = POPUP_MIN_W;
	int32_t height = 2 * POPUP_PAD;
	int32_t y = POPUP_PAD;
	int32_t x;

	lv_obj_clean(popup.panel);
	popup.dismissed = false;

	for (uint8_t i = 0; i < m->item_count; i++) {
		if (m->items[i].separator) {
			height += SEP_H;
			continue;
		}
		width = MAX(width, label_width(popup.panel, m->items[i].label) +
					   2 * ITEM_PAD);
		height += ITEM_H;
	}

	lv_obj_set_size(popup.panel, width, height);

	for (uint8_t i = 0; i < m->item_count; i++) {
		lv_obj_t *row;
		lv_obj_t *label;

		if (m->items[i].separator) {
			/* Two lines, dark over light: the Win95 etched groove,
			 * and the cheapest thing that reads as a divider rather
			 * than as an item nobody can click.
			 */
			row = lv_obj_create(popup.panel);
			lv_obj_remove_style_all(row);
			lv_obj_set_size(row, width - 2 * POPUP_PAD, 2);
			lv_obj_set_pos(row, POPUP_PAD, y + 1);
			lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
			zd_bevel_attach(row, ZD_BEVEL_IN);
			y += SEP_H;
			continue;
		}

		row = lv_obj_create(popup.panel);
		lv_obj_remove_style_all(row);
		lv_obj_add_style(row, &zd_style_face, LV_PART_MAIN);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_size(row, width - 2 * POPUP_PAD, ITEM_H);
		lv_obj_set_pos(row, POPUP_PAD, y);
		y += ITEM_H;

		if (m->items[i].enabled) {
			lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
			lv_obj_add_event_cb(row, item_clicked, LV_EVENT_CLICKED,
					    (void *)(uintptr_t)i);
		} else {
			lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
		}

		label = lv_label_create(row);
		lv_label_set_text(label, m->items[i].label);
		lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
		lv_obj_set_style_text_color(label,
					    lv_color_hex(m->items[i].enabled ? ZD_C_TEXT
									     : ZD_C_SHADOW),
					    LV_PART_MAIN);
		lv_obj_set_pos(label, ITEM_PAD, (ITEM_H - 12) / 2);
	}

	/* Under its title, nudged back onto the screen if it would hang off. */
	lv_obj_get_coords(m->title, &anchor);
	x = MIN(anchor.x1, screen_w - width);
	lv_obj_set_pos(popup.panel, MAX(x, 0),
		       MIN(anchor.y2 + 1, screen_h - height));

	lv_obj_remove_flag(popup.shade, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(popup.panel, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_to_index(popup.shade, -1);
	lv_obj_move_to_index(popup.panel, -1);

	popup.open = m;
}

static void title_clicked(lv_event_t *e)
{
	struct menu *m = lv_event_get_user_data(e);

	if (popup.open == m) {
		zd_menu_close();
		return;
	}

	build_popup(m);
}

/* --- the bar -------------------------------------------------------------------- */

uintptr_t zd_menubar_create(struct zd_zapp_instance *owner, struct zd_client *client)
{
	struct menu *bar;

	if (client->menubar != NULL) {
		return 0; /* one bar per window; asking twice is a zapp bug */
	}

	bar = claim();
	if (bar == NULL) {
		LOG_WRN("menu table full (%d)", CONFIG_ZD_MAX_MENUS);
		return 0;
	}

	bar->used = true;
	bar->is_bar = true;
	bar->owner = owner;
	bar->client = client;

	bar->handle = zd_handle_alloc(ZD_HANDLE_MENU, bar, owner);
	if (bar->handle == 0) {
		bar->used = false;
		return 0;
	}

	live_menus++;

	client->menubar = lv_obj_create(client->frame);
	lv_obj_remove_style_all(client->menubar);
	lv_obj_add_style(client->menubar, &zd_style_face, LV_PART_MAIN);
	lv_obj_remove_flag(client->menubar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(client->menubar, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_add_event_cb(client->menubar, bar_deleted, LV_EVENT_DELETE, bar);
	client->menubar_h = ZD_MENUBAR_H;

	/* The content area just got shorter. Re-lay the subtree out and say so,
	 * or a zapp that made its widgets before adding a menu is wrong and has
	 * no way to find out.
	 */
	zd_client_apply_geom(client);
	zd_wm_notify_resized(client);

	return bar->handle;
}

uintptr_t zd_menu_add_submenu(struct zd_zapp_instance *owner, uintptr_t bar_handle,
			      const char *label)
{
	struct menu *bar = menu_of(owner, bar_handle);
	struct menu *sub;
	lv_obj_t *text;
	int32_t x = 0;

	if (bar == NULL || !bar->is_bar || label == NULL ||
	    bar->sub_count >= CONFIG_ZD_MENU_MAX_SUBMENUS) {
		return 0;
	}

	sub = claim();
	if (sub == NULL) {
		return 0;
	}

	sub->used = true;
	sub->owner = owner;
	sub->client = bar->client;
	sub->bar = bar;

	sub->handle = zd_handle_alloc(ZD_HANDLE_MENU, sub, owner);
	if (sub->handle == 0) {
		sub->used = false;
		return 0;
	}

	live_menus++;

	for (uint8_t i = 0; i < bar->sub_count; i++) {
		x += lv_obj_get_width(bar->subs[i]->title);
	}

	/* Titles sit edge to edge, so they are made wide rather than given hit
	 * area they do not occupy.
	 */
	sub->title = lv_obj_create(bar->client->menubar);
	lv_obj_remove_style_all(sub->title);
	lv_obj_add_style(sub->title, &zd_style_face, LV_PART_MAIN);
	lv_obj_remove_flag(sub->title, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(sub->title, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(sub->title,
			label_width(bar->client->menubar, label) + 2 * TITLE_PAD,
			ZD_MENUBAR_H);
	lv_obj_set_pos(sub->title, x, 0);
	lv_obj_add_event_cb(sub->title, title_clicked, LV_EVENT_CLICKED, sub);

	text = lv_label_create(sub->title);
	lv_label_set_text(text, label);
	lv_obj_set_style_text_font(text, &lv_font_montserrat_12, LV_PART_MAIN);
	lv_obj_set_style_text_color(text, lv_color_hex(ZD_C_TEXT), LV_PART_MAIN);
	lv_obj_center(text);

	bar->subs[bar->sub_count++] = sub;

	return sub->handle;
}

static int add(struct zd_zapp_instance *owner, uintptr_t handle, const char *label,
	       uint16_t id, bool separator)
{
	struct menu *m = menu_of(owner, handle);

	if (m == NULL || m->is_bar || m->item_count >= CONFIG_ZD_MENU_MAX_ITEMS) {
		return -EINVAL;
	}

	struct menu_item *item = &m->items[m->item_count++];

	item->id = id;
	item->separator = separator;
	item->enabled = true;

	if (label != NULL) {
		strncpy(item->label, label, sizeof(item->label) - 1);
	}

	return 0;
}

int zd_menu_add_item(struct zd_zapp_instance *owner, uintptr_t menu, const char *label,
		     uint16_t id)
{
	return label != NULL ? add(owner, menu, label, id, false) : -EINVAL;
}

int zd_menu_add_separator(struct zd_zapp_instance *owner, uintptr_t menu)
{
	return add(owner, menu, NULL, 0, true);
}

int zd_menu_set_item_enabled(struct zd_zapp_instance *owner, uintptr_t handle, uint16_t id,
			     bool enabled)
{
	struct menu *m = menu_of(owner, handle);

	if (m == NULL || m->is_bar) {
		return -EINVAL;
	}

	for (uint8_t i = 0; i < m->item_count; i++) {
		if (!m->items[i].separator && m->items[i].id == id) {
			m->items[i].enabled = enabled;
			/* If this menu is on screen it is now wrong. Rebuilding
			 * is what launcher.c does on every open and costs
			 * nothing at these sizes.
			 */
			if (popup.open == m) {
				build_popup(m);
			}
			return 0;
		}
	}

	return -ENOENT;
}

bool zd_menu_title_coords(uintptr_t bar_handle, uint32_t index, lv_area_t *out)
{
	struct menu *bar = zd_handle_deref(bar_handle, ZD_HANDLE_MENU, NULL);

	if (bar == NULL || !bar->is_bar || index >= bar->sub_count) {
		return false;
	}

	lv_obj_get_coords(bar->subs[index]->title, out);
	return true;
}

uint32_t zd_menu_live_count(void)
{
	return live_menus;
}

void zd_menu_init(struct zd_layers *layers)
{
	/*
	 * The shade is what makes "click somewhere else to dismiss" work
	 * without every other surface having to know a menu might be open. It
	 * is fully transparent and fully clickable, so the press that misses
	 * the menu is eaten here rather than also raising a window -- which is
	 * what Win95 did, and is the same mechanism a modal dialog needs.
	 */
	popup.shade = lv_obj_create(layers->overlay);
	lv_obj_remove_style_all(popup.shade);
	lv_obj_set_size(popup.shade, LV_PCT(100), LV_PCT(100));
	lv_obj_set_pos(popup.shade, 0, 0);
	lv_obj_add_flag(popup.shade, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(popup.shade, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(popup.shade, shade_clicked, LV_EVENT_PRESSED, NULL);

	popup.panel = lv_obj_create(layers->overlay);
	lv_obj_remove_style_all(popup.panel);
	lv_obj_add_style(popup.panel, &zd_style_face, LV_PART_MAIN);
	lv_obj_remove_flag(popup.panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(popup.panel, LV_OBJ_FLAG_HIDDEN);
	zd_bevel_attach(popup.panel, ZD_BEVEL_OUT);
}
