/*
 * zephyr-desktop — background and layer construction.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "desktop.h"
#include "../chrome/theme.h"

static lv_obj_t *bare_child(lv_obj_t *parent)
{
	lv_obj_t *obj = lv_obj_create(parent);

	lv_obj_remove_style_all(obj);
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
	return obj;
}

void zd_desktop_init(struct zd_layers *layers)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
	lv_obj_set_style_bg_color(scr, lv_color_hex(ZD_C_DESKTOP), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

	/* Background. Clickable so that a press on bare desktop is a real event
	 * the WM can turn into "defocus everything", rather than a press that
	 * lands nowhere.
	 */
	layers->desktop = bare_child(scr);
	lv_obj_set_size(layers->desktop, LV_PCT(100), LV_PCT(100));
	lv_obj_set_pos(layers->desktop, 0, 0);
	lv_obj_set_style_bg_color(layers->desktop, lv_color_hex(ZD_C_DESKTOP), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(layers->desktop, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_image_src(layers->desktop, &zd_desktop_pattern, LV_PART_MAIN);
	lv_obj_set_style_bg_image_tiled(layers->desktop, true, LV_PART_MAIN);
	lv_obj_add_flag(layers->desktop, LV_OBJ_FLAG_CLICKABLE);

	/* Window layer. Full-screen and deliberately NOT clickable: LVGL's hit
	 * search still descends into its children, but the layer itself never
	 * becomes the target, so a press on empty space falls through to the
	 * background beneath instead of being swallowed here.
	 */
	layers->windows = bare_child(scr);
	lv_obj_set_size(layers->windows, LV_PCT(100), LV_PCT(100));
	lv_obj_set_pos(layers->windows, 0, 0);
	lv_obj_remove_flag(layers->windows, LV_OBJ_FLAG_CLICKABLE);

	/* Panel. Last child, so permanently on top. Populated at milestone B. */
	layers->panel = bare_child(scr);
	lv_obj_set_size(layers->panel, LV_PCT(100), ZD_TASKBAR_H);
	lv_obj_set_pos(layers->panel, 0, lv_display_get_vertical_resolution(NULL) - ZD_TASKBAR_H);
	lv_obj_add_style(layers->panel, &zd_style_face, LV_PART_MAIN);
	zd_bevel_attach(layers->panel, ZD_BEVEL_OUT);
}
