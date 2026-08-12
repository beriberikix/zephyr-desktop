/*
 * zephyr-desktop — menu bars and their drop-downs.
 *
 * Drawn by the desktop, not by the zapp. A menu is chrome: it has to match the
 * palette, it has to behave the same in every application, and on a touch panel
 * it has to obey the sizing rule this project has now learned three times. A
 * zapp says what the items are and gets a command id back when one is chosen.
 *
 * The bar lives inside the window, between the titlebar and the content area,
 * and the content area shrinks to make room -- so a zapp's coordinates stay
 * relative to the area below the menu and it never has to know the menu is
 * there. Windows without a menu are laid out exactly as they were before.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_CHROME_MENU_H_
#define ZD_CHROME_MENU_H_

#include <stdbool.h>
#include <stdint.h>

#include "../shell/desktop.h"

struct zd_client;
struct zd_zapp_instance;

/** Build the shared drop-down surface on the overlay. Call once at boot. */
void zd_menu_init(struct zd_layers *layers);

/**
 * @brief Give a window a menu bar.
 *
 * The content area shrinks by the bar's height and the owner is told, so a zapp
 * that adds a menu after laying out its widgets is not left guessing.
 *
 * @return a handle, or 0.
 */
uintptr_t zd_menubar_create(struct zd_zapp_instance *owner, struct zd_client *client);

/** @return a handle to the new drop-down, or 0. */
uintptr_t zd_menu_add_submenu(struct zd_zapp_instance *owner, uintptr_t bar,
			      const char *label);

int zd_menu_add_item(struct zd_zapp_instance *owner, uintptr_t menu, const char *label,
		     uint16_t id);
int zd_menu_add_separator(struct zd_zapp_instance *owner, uintptr_t menu);
int zd_menu_set_item_enabled(struct zd_zapp_instance *owner, uintptr_t menu, uint16_t id,
			     bool enabled);

/** Dismiss whatever drop-down is open. Safe from inside dispatch. */
void zd_menu_close(void);

/**
 * @brief Delete the dismissed drop-down's widgets.
 *
 * Called from the desktop loop, never from dispatch -- choosing a menu item
 * fires an event the zapp may answer by closing its window, and the row that
 * was clicked must still exist when that returns. The same rule as the taskbar
 * rebuild, met from a third direction.
 */
void zd_menu_reap(void);

/** Live menu objects, bars and drop-downs together. For leak assertions. */
uint32_t zd_menu_live_count(void);

/** Screen rectangle of a bar's @p index'th title button. For the boot check. */
bool zd_menu_title_coords(uintptr_t bar, uint32_t index, lv_area_t *out);

#endif /* ZD_CHROME_MENU_H_ */
