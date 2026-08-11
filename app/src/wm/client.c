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

/*
 * One titlebar button.
 *
 * Note what is absent: lv_obj_set_ext_click_area(). ZD_BTN_SZ already carries
 * the touch allowance as real pixels, because these two sit side by side and
 * overlapping hit areas would hand every tap to whichever was added last. See
 * the comment on the constants in wm.h.
 */
static lv_obj_t *title_button(struct zd_client *client)
{
	lv_obj_t *btn = bare(client->titlebar);

	lv_obj_add_style(btn, &zd_style_face, LV_PART_MAIN);
	lv_obj_set_size(btn, ZD_BTN_SZ, ZD_BTN_SZ);
	lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
	zd_bevel_attach(btn, ZD_BEVEL_BUTTON);
	return btn;
}

/*
 * Position and size every part of the subtree from client->geom.
 *
 * Split out of zd_client_build() because resize needs exactly this and nothing
 * else. Keeping one copy of the arithmetic is the difference between resize
 * being three lines and resize being a place where the chrome slowly drifts out
 * of agreement with itself.
 */
void zd_client_content_size(const struct zd_client *client, int16_t *w, int16_t *h)
{
	*w = (int16_t)(lv_area_get_width(&client->geom) - 2 * ZD_FRAME_PAD);
	*h = (int16_t)(lv_area_get_height(&client->geom) - 2 * ZD_FRAME_PAD - ZD_TITLEBAR_H -
		       ZD_CONTENT_GAP);
}

static void layout_subtree(struct zd_client *client)
{
	int32_t w = lv_area_get_width(&client->geom);
	int32_t h = lv_area_get_height(&client->geom);
	int32_t inner_w = w - 2 * ZD_FRAME_PAD;
	int32_t btn_y = (ZD_TITLEBAR_H - ZD_BTN_SZ) / 2;
	int16_t content_w;
	int16_t content_h;

	lv_obj_set_size(client->frame, w, h);
	lv_obj_set_pos(client->frame, client->geom.x1, client->geom.y1);

	lv_obj_set_size(client->titlebar, inner_w, ZD_TITLEBAR_H);
	lv_obj_set_pos(client->titlebar, ZD_FRAME_PAD, ZD_FRAME_PAD);

	/* Two buttons now. MAX because ZD_WIN_MIN_W does not by itself
	 * guarantee room for the title once the buttons have taken their share,
	 * and a negative width is an LVGL assert rather than a small label.
	 */
	lv_obj_set_width(client->title_label, MAX(inner_w - 2 * ZD_BTN_SZ - 10, 1));
	lv_obj_set_pos(client->title_label, 3, (ZD_TITLEBAR_H - 12) / 2);

	lv_obj_set_pos(client->min_btn, inner_w - 2 * ZD_BTN_SZ - 4, btn_y);
	lv_obj_set_pos(client->close_btn, inner_w - ZD_BTN_SZ - 2, btn_y);

	zd_client_content_size(client, &content_w, &content_h);
	lv_obj_set_size(client->content, content_w, content_h);
	lv_obj_set_pos(client->content, ZD_FRAME_PAD,
		       ZD_FRAME_PAD + ZD_TITLEBAR_H + ZD_CONTENT_GAP);

	lv_obj_set_pos(client->grip, w - ZD_FRAME_PAD - ZD_GRIP_SZ,
		       h - ZD_FRAME_PAD - ZD_GRIP_SZ);
}

void zd_client_build(struct zd_client *client, lv_obj_t *parent)
{
	/* Frame. Clickable so a press anywhere on the window -- including on the
	 * padding between chrome elements -- is a real event the WM can turn
	 * into raise+focus.
	 */
	client->frame = bare(parent);
	lv_obj_add_style(client->frame, &zd_style_face, LV_PART_MAIN);
	lv_obj_add_flag(client->frame, LV_OBJ_FLAG_CLICKABLE);
	zd_bevel_attach(client->frame, ZD_BEVEL_OUT);

	/* Titlebar. */
	client->titlebar = bare(client->frame);
	lv_obj_add_flag(client->titlebar, LV_OBJ_FLAG_CLICKABLE);
	zd_titlebar_set_active(client->titlebar, false);

	client->title_label = lv_label_create(client->titlebar);
	lv_label_set_long_mode(client->title_label, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(client->title_label, client->title);

	/* Minimise then close, left to right, hard right in the titlebar. */
	client->min_btn = title_button(client);
	zd_minimize_glyph_attach(client->min_btn);

	client->close_btn = title_button(client);
	zd_close_glyph_attach(client->close_btn);

	/* Content area: where a zapp's widgets go. Sunken, like a Win95 client
	 * area, and white so text drawn by a zapp reads.
	 */
	client->content = bare(client->frame);
	lv_obj_set_style_bg_color(client->content, lv_color_hex(ZD_C_LIGHT), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(client->content, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_text_color(client->content, lv_color_hex(ZD_C_TEXT), LV_PART_MAIN);
	lv_obj_set_style_text_font(client->content, &lv_font_montserrat_14, LV_PART_MAIN);
	zd_bevel_attach(client->content, ZD_BEVEL_IN);

	/* Resize grip, over the content's bottom-right corner. Added last on
	 * purpose: LVGL awards an overlap to the last child, so the grip wins
	 * the corner against the content area underneath it.
	 *
	 * Which is also why it has no ext_click_area. Slop here would be slop
	 * over the zapp's content -- an invisible dead zone in the corner of
	 * every window, swallowing clicks the zapp was expecting. ZD_GRIP_SZ
	 * carries the touch allowance as pixels the user can actually see.
	 */
	client->grip = bare(client->frame);
	lv_obj_add_style(client->grip, &zd_style_face, LV_PART_MAIN);
	lv_obj_set_size(client->grip, ZD_GRIP_SZ, ZD_GRIP_SZ);
	lv_obj_add_flag(client->grip, LV_OBJ_FLAG_CLICKABLE);
	zd_bevel_attach(client->grip, ZD_BEVEL_OUT);

	layout_subtree(client);
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
	client->min_btn = NULL;
	client->close_btn = NULL;
	client->content = NULL;
	client->grip = NULL;
}

void zd_client_apply_geom(struct zd_client *client)
{
	/* Re-lays out everything, not just the frame's position: this is the one
	 * direction geometry is allowed to flow. client->geom is the model and
	 * LVGL is told what it now says -- never read back and believed.
	 */
	layout_subtree(client);
}
