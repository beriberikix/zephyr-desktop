/*
 * zephyr-desktop — building and tearing down a client's LVGL subtree.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "client.h"
#include "../chrome/theme.h"
#include "../chrome/titlebar.h"

static lv_obj_t *bare(lv_obj_t *parent)
{
	lv_obj_t *obj = lv_obj_create(parent);

	lv_obj_remove_style_all(obj);
	lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
	return obj;
}

void zd_client_build(struct zd_client *client, lv_obj_t *parent)
{
	int32_t w = lv_area_get_width(&client->geom);
	int32_t h = lv_area_get_height(&client->geom);
	int32_t inner_w = w - 2 * ZD_FRAME_PAD;

	/* Frame. Clickable so a press anywhere on the window -- including on the
	 * padding between chrome elements -- is a real event the WM can turn
	 * into raise+focus.
	 */
	client->frame = bare(parent);
	lv_obj_add_style(client->frame, &zd_style_face, LV_PART_MAIN);
	lv_obj_set_size(client->frame, w, h);
	lv_obj_set_pos(client->frame, client->geom.x1, client->geom.y1);
	lv_obj_add_flag(client->frame, LV_OBJ_FLAG_CLICKABLE);
	zd_bevel_attach(client->frame, ZD_BEVEL_OUT);

	/* Titlebar. */
	client->titlebar = bare(client->frame);
	lv_obj_set_size(client->titlebar, inner_w, ZD_TITLEBAR_H);
	lv_obj_set_pos(client->titlebar, ZD_FRAME_PAD, ZD_FRAME_PAD);
	lv_obj_add_flag(client->titlebar, LV_OBJ_FLAG_CLICKABLE);
	zd_titlebar_set_active(client->titlebar, false);

	client->title_label = lv_label_create(client->titlebar);
	lv_label_set_long_mode(client->title_label, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_width(client->title_label, inner_w - ZD_CLOSE_SZ - 8);
	lv_obj_set_pos(client->title_label, 3, (ZD_TITLEBAR_H - 12) / 2);
	lv_label_set_text(client->title_label, client->title);

	/* Close button, hard right inside the titlebar. */
	client->close_btn = bare(client->titlebar);
	lv_obj_add_style(client->close_btn, &zd_style_face, LV_PART_MAIN);
	lv_obj_set_size(client->close_btn, ZD_CLOSE_SZ, ZD_CLOSE_SZ);
	lv_obj_set_pos(client->close_btn, inner_w - ZD_CLOSE_SZ - 2,
		       (ZD_TITLEBAR_H - ZD_CLOSE_SZ) / 2);
	lv_obj_add_flag(client->close_btn, LV_OBJ_FLAG_CLICKABLE);
	zd_bevel_attach(client->close_btn, ZD_BEVEL_BUTTON);
	zd_close_glyph_attach(client->close_btn);
	/* 14x14 is a mouse-sized target; a fingertip needs help. */
	lv_obj_set_ext_click_area(client->close_btn, CONFIG_ZD_TOUCH_SLOP_PX);

	/* Content area: where a zapp's widgets go. Sunken, like a Win95 client
	 * area, and white so text drawn by a zapp reads.
	 */
	client->content = bare(client->frame);
	lv_obj_set_size(client->content, inner_w,
			h - 2 * ZD_FRAME_PAD - ZD_TITLEBAR_H - ZD_CONTENT_GAP);
	lv_obj_set_pos(client->content, ZD_FRAME_PAD,
		       ZD_FRAME_PAD + ZD_TITLEBAR_H + ZD_CONTENT_GAP);
	lv_obj_set_style_bg_color(client->content, lv_color_hex(ZD_C_LIGHT), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(client->content, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_text_color(client->content, lv_color_hex(ZD_C_TEXT), LV_PART_MAIN);
	lv_obj_set_style_text_font(client->content, &lv_font_montserrat_14, LV_PART_MAIN);
	zd_bevel_attach(client->content, ZD_BEVEL_IN);
}

void zd_client_destroy_widgets(struct zd_client *client)
{
	if (client->frame != NULL) {
		/* Deleting the frame takes the whole subtree with it. */
		lv_obj_delete(client->frame);
	}
	client->frame = NULL;
	client->titlebar = NULL;
	client->title_label = NULL;
	client->close_btn = NULL;
	client->content = NULL;
}

void zd_client_apply_geom(struct zd_client *client)
{
	lv_obj_set_pos(client->frame, client->geom.x1, client->geom.y1);
}
