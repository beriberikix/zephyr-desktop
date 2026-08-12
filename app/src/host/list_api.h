/*
 * zephyr-desktop — the list widget, as zapps see it.
 *
 * chrome/rowlist.c is the widget; this is the layer that gives it handles,
 * ownership and a way to reach a zapp. Exactly the arrangement text_api.c has
 * over lv_textarea, and split for the same reason: the file picker wants the
 * widget without any of this.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_LIST_API_H_
#define ZD_HOST_LIST_API_H_

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>
#include <zd/zapp_abi.h>

struct zd_client;
struct zd_zapp_instance;

uintptr_t zd_list_create(struct zd_zapp_instance *owner, struct zd_client *client,
			 const struct zd_rect *geom, uint32_t flags);
void zd_list_destroy(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_list_set_geometry(struct zd_zapp_instance *owner, uintptr_t handle,
			 const struct zd_rect *geom);

int zd_list_clear(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_list_add_item(struct zd_zapp_instance *owner, uintptr_t handle, const char *text,
		     uint16_t id);
int zd_list_get_count(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_list_get_capacity(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_list_get_selected(struct zd_zapp_instance *owner, uintptr_t handle);
int zd_list_set_selected(struct zd_zapp_instance *owner, uintptr_t handle, int32_t index);
int zd_list_get_item_id(struct zd_zapp_instance *owner, uintptr_t handle, int32_t index);
int zd_list_get_item_text(struct zd_zapp_instance *owner, uintptr_t handle, int32_t index,
			  char *buf, uint32_t len);

/**
 * @brief Give a list the keyboard within its window.
 *
 * The counterpart of zd_text_focus(), and the two clear each other: one window
 * has one place keys go.
 */
void zd_list_focus(struct zd_client *client, lv_obj_t *view);

/** Installed as wm->on_client_list_key. @return true if the list used the key. */
bool zd_list_on_client_key(struct zd_client *client, uint32_t code, uint32_t unicode,
			   uint16_t mods);

/** Live list widgets, for leak assertions. */
uint32_t zd_list_live_count(void);

#endif /* ZD_HOST_LIST_API_H_ */
