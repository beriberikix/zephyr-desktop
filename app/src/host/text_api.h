/*
 * zephyr-desktop — the editable text widget behind zd_text_t.
 *
 * A zapp never sees the lv_textarea any more than it sees the lv_obj_t behind a
 * window: it gets a generation-counted handle out of the same registry, checked
 * for liveness, kind and ownership on every call.
 *
 * What the desktop gets in exchange for using LVGL's textarea rather than
 * writing an editor: a blinking caret, word wrap, scrolling, and
 * click-to-position, none of which is interesting to write and all of which is
 * tedious to get right.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_TEXT_API_H_
#define ZD_HOST_TEXT_API_H_

#include <stdbool.h>
#include <stdint.h>

#include <zd/zapp_abi.h>

#include "../wm/wm.h"

struct zd_zapp_instance;

/**
 * @brief Create a text widget in a window's content area.
 *
 * @return its handle, or 0 if the table is full or the window is not the
 *         caller's.
 */
uintptr_t zd_text_create(struct zd_zapp_instance *owner, struct zd_client *client,
			 const struct zd_rect *geom, uint32_t flags);

void zd_text_destroy(struct zd_zapp_instance *owner, uintptr_t handle);

/**
 * @brief Give @p ta the caret within its window, or pass NULL to take it away.
 *
 * Keyboard focus *within* a window is the window manager's business in the same
 * way focus between windows is, which is why the state lives on struct
 * zd_client rather than in a table here.
 */
void zd_text_focus(struct zd_client *client, lv_obj_t *ta);

int zd_text_set_text(struct zd_zapp_instance *owner, uintptr_t handle, const char *s);

/**
 * @brief Copy out from byte offset @p from.
 *
 * Short by contract, like zd_fs_read: a document can outgrow any sane single
 * call, so the zapp loops. @return bytes copied, 0 at the end, or -errno.
 */
int zd_text_get_text(struct zd_zapp_instance *owner, uintptr_t handle, uint32_t from,
		     char *buf, uint32_t len);

int zd_text_get_length(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_text_get_capacity(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_text_insert(struct zd_zapp_instance *owner, uintptr_t handle, const char *s);
int zd_text_set_geometry(struct zd_zapp_instance *owner, uintptr_t handle,
			 const struct zd_rect *geom);
int zd_text_set_cursor(struct zd_zapp_instance *owner, uintptr_t handle, uint32_t pos);
int zd_text_get_cursor(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_text_get_selection(struct zd_zapp_instance *owner, uintptr_t handle, uint32_t *from,
			  uint32_t *to);
int zd_text_select(struct zd_zapp_instance *owner, uintptr_t handle, uint32_t from,
		   uint32_t to);
int zd_text_delete_selection(struct zd_zapp_instance *owner, uintptr_t handle);

/**
 * @brief The three clipboard verbs, against a text widget.
 *
 * Here rather than in each zapp because the empty-selection case of each is
 * where a reimplementation goes wrong, and because two zapps should agree about
 * what Ctrl+V does.
 *
 * @return bytes moved, 0 for a no-op (nothing selected, empty clipboard), or a
 *         negative errno.
 */
int zd_text_copy(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_text_cut(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_text_paste(struct zd_zapp_instance *owner, uintptr_t handle);

/**
 * @brief Offer a key to the focused client's text widget.
 *
 * Installed as wm->on_client_text_key. Returns true when the widget consumed
 * the key, in which case the zapp is not told about it. Keys it has no meaning
 * for -- Escape, the function keys -- are declined so they reach the zapp as
 * ZD_EV_KEY.
 */
bool zd_text_on_client_key(struct zd_client *client, uint32_t code, uint32_t unicode,
			   uint16_t mods);

/**
 * @brief Apply a key to a bare lv_textarea.
 *
 * The editing behaviour without the handle, the ownership check or the change
 * event, for desktop-owned fields that are not a zapp's -- the filename box in
 * the save dialog is the only one so far. Exported so a second implementation
 * of "what Backspace does" cannot drift away from the first.
 */
bool zd_text_key_obj(lv_obj_t *ta, uint32_t code, uint32_t unicode, uint16_t mods);

/** Live text widgets, desktop-wide. For leak assertions. */
uint32_t zd_text_live_count(void);

#endif /* ZD_HOST_TEXT_API_H_ */
