/*
 * zephyr-desktop — an assistant balloon: a stock icon and a coloured panel.
 *
 * This is the first thing in the desktop that draws a *picture*. Every widget
 * before it is grey chrome with text in it, which was fine until a zapp wanted
 * a paperclip.
 *
 * The picture is drawn here rather than shipped as one, and that is the whole
 * design. An image call in the ABI would mean a pixel format, a decoder, a
 * lifetime for the pixels and a way for a zapp with no libc to build them --
 * a great deal of machinery, and zapps/clippy/face.c said so at the time. A
 * stock icon the desktop draws needs none of it: the zapp names a picture and
 * the desktop owns the artwork, which is exactly the bargain a Win95
 * MessageBox strikes with its information and warning icons. ZD_ICON_CLIP sits
 * beside them as a third.
 *
 * Built from lv_draw_rect alone -- rounded rectangles with a border and no
 * fill are wire loops, and rectangles with LV_RADIUS_CIRCLE are eyes. Arcs and
 * triangles would have read better in two or three places, but nothing else in
 * this tree draws one, and a widget that cannot be compiled is worse looking
 * than a paperclip made of stadiums.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_CHROME_BALLOON_H_
#define ZD_CHROME_BALLOON_H_

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

struct zd_balloon;

/** Width the icon column occupies, including the gap before the panel. */
int16_t zd_balloon_icon_width(void);

/** Height below which the icon is not worth drawing at all. */
int16_t zd_balloon_icon_height(void);

/**
 * @brief Create a balloon inside @p parent.
 *
 * @param icon  one of the ZD_ICON_* values from the ABI.
 * @return NULL if the table is full.
 */
struct zd_balloon *zd_balloon_create(lv_obj_t *parent, int16_t x, int16_t y, int16_t w,
				     int16_t h, uint32_t icon);
void zd_balloon_destroy(struct zd_balloon *b);

int zd_balloon_set_text(struct zd_balloon *b, const char *text);
int zd_balloon_set_geometry(struct zd_balloon *b, int16_t x, int16_t y, int16_t w,
			    int16_t h);

/**
 * @brief Change which picture is drawn, and its expression.
 *
 * @param state 0..3. Only ZD_ICON_CLIP has more than one; the others ignore it.
 *              This is what animates the paperclip, and it is a state rather
 *              than a frame number so the caller never has to know how many
 *              frames a mood has.
 */
int zd_balloon_set_icon(struct zd_balloon *b, uint32_t icon, uint32_t state);

lv_obj_t *zd_balloon_obj(const struct zd_balloon *b);

/** Live balloons, for leak assertions. */
uint32_t zd_balloon_live_count(void);

#endif /* ZD_CHROME_BALLOON_H_ */
