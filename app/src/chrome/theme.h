/*
 * zephyr-desktop — the one hardcoded retro theme.
 *
 * There is deliberately no theming engine. One palette, one bevel style, shared
 * by every window and every button.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_CHROME_THEME_H_
#define ZD_CHROME_THEME_H_

#include <lvgl.h>

/* The Windows 95 system palette, near enough. */
#define ZD_C_DESKTOP        0x008080 /* teal */
#define ZD_C_DESKTOP_DITHER 0x006060
#define ZD_C_FACE           0xC0C0C0 /* button face */
#define ZD_C_LIGHT          0xFFFFFF /* outer highlight */
#define ZD_C_FACE_LIGHT     0xDFDFDF /* inner highlight */
#define ZD_C_SHADOW         0x808080 /* inner shadow */
#define ZD_C_DARK           0x000000 /* outer shadow */
#define ZD_C_TITLE_ACTIVE   0x000080 /* navy */
#define ZD_C_TITLE_INACTIVE 0x808080
#define ZD_C_TITLE_TEXT     0xFFFFFF
#define ZD_C_TEXT           0x000000
/* A selected row in a list. The same navy an active titlebar uses, because in
 * this era they were the same colour -- "highlight" was one system setting.
 */
#define ZD_C_SELECT         ZD_C_TITLE_ACTIVE
#define ZD_C_SELECT_TEXT    0xFFFFFF

/** Thickness of a bevel ring pair, in pixels. */
#define ZD_BEVEL_W 2

typedef enum {
	ZD_BEVEL_OUT,    /**< raised: light top-left, dark bottom-right */
	ZD_BEVEL_IN,     /**< sunken: the reverse */
	ZD_BEVEL_BUTTON, /**< raised, but sunken while LV_STATE_PRESSED */
} zd_bevel_t;

/** Build the shared styles. Call once, before any chrome is created. */
void zd_theme_init(void);

/**
 * @brief Give @p obj a two-ring Win95 bevel.
 *
 * Drawn in LV_EVENT_DRAW_POST rather than expressed as border styles: an LVGL
 * style carries a single border colour, so a two-tone edge would otherwise need
 * nested objects per bevel. Drawing it keeps one object per visual element and
 * stays entirely behind LVGL's draw layer, so a GPU draw unit still applies.
 */
void zd_bevel_attach(lv_obj_t *obj, zd_bevel_t kind);

/** Flat #C0C0C0 panel: no radius, no border, no padding, not scrollable. */
extern lv_style_t zd_style_face;

/** 8x8 dithered teal, tiled across the desktop background. */
extern const lv_image_dsc_t zd_desktop_pattern;

#endif /* ZD_CHROME_THEME_H_ */
