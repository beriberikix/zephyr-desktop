/*
 * zephyr-desktop — window manager data model.
 *
 * Modelled on a tiny X11 stacking WM: a client struct per window, a single
 * stacking list, and one central dispatch path. The list is the truth; LVGL's
 * child order is a projection of it, re-applied by zd_wm_restack().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_WM_WM_H_
#define ZD_WM_WM_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/slist.h>

#include <lvgl.h>

#include "../shell/desktop.h"

#define ZD_TITLE_MAX 32

/* Chrome geometry, in pixels. */
#define ZD_FRAME_PAD   3  /**< frame edge to titlebar/content */
#define ZD_TITLEBAR_H  18
#define ZD_CLOSE_SZ    14
#define ZD_CONTENT_GAP 2  /**< titlebar to content */

#define ZD_WIN_MIN_W (ZD_FRAME_PAD * 2 + 60)
#define ZD_WIN_MIN_H (ZD_FRAME_PAD * 2 + ZD_TITLEBAR_H + ZD_CONTENT_GAP + 20)

struct zd_zapp_instance; /* milestone F */
struct zd_wm;

/**
 * One managed window.
 *
 * Allocated from a fixed slab rather than the heap: the bound is the point. A
 * leaked window shows up immediately as slab exhaustion instead of as slow heap
 * growth that nobody notices until the "no leaks" criterion is being measured.
 */
struct zd_client {
	sys_dnode_t node;      /**< in wm->stack; head == topmost */
	sys_snode_t reap_node; /**< in wm->reap_list when pending_destroy */
	uint32_t id;

	/* LVGL subtree. Owned by the WM; never handed to a zapp. */
	lv_obj_t *frame; /**< child of layers->windows */
	lv_obj_t *titlebar;
	lv_obj_t *title_label;
	lv_obj_t *close_btn;
	lv_obj_t *content; /**< the app's area */

	/* WM-authoritative geometry. LVGL follows this, never the reverse. */
	lv_area_t geom;
	char title[ZD_TITLE_MAX];

	bool focused;
	bool pending_destroy;

	struct zd_zapp_instance *owner; /**< NULL == desktop-internal window */
	uintptr_t handle;              /**< the app's handle for this window, or 0 */

	/* Drag state, valid only while dragging. */
	lv_point_t drag_grab;   /**< pointer position at press */
	lv_point_t drag_origin; /**< window position at press */
	bool dragging;

	struct zd_wm *wm;
};

struct zd_wm {
	struct zd_layers *layers;
	sys_dlist_t stack; /**< z-order, front to back */
	sys_slist_t reap_list;
	struct zd_client *focused;
	uint32_t next_id;

	/* Depth of zapp callbacks currently on the stack. Teardown that would
	 * free code the return address points into must wait for this to hit 0.
	 */
	uint32_t in_zapp_callback;

	/* Called from the reap once a client's widgets are gone, before its slab
	 * block is released. A hook rather than a direct call into the loader:
	 * the WM has no business knowing that zapps exist.
	 */
	void (*on_client_destroyed)(struct zd_client *client);

	/* Called when a client gains or loses focus, so the loader can turn it
	 * into a ZD_EV_WINDOW_FOCUS / _BLUR for the owning zapp.
	 */
	void (*on_client_focus)(struct zd_client *client, bool focused);
};

void zd_wm_init(struct zd_wm *wm, struct zd_layers *layers);

/**
 * @brief Create a managed window.
 *
 * The window is stacked on top but NOT focused: the caller must attach any
 * ownership and handle first, then call zd_wm_focus(), or the owning zapp's
 * focus event is dispatched before it has a handle and is dropped.
 *
 * @param geom desired outer rectangle; w/h are clamped to the window minimum,
 *             and a w or h of 0 means "pick a default".
 * @return the client, or NULL if the slab is exhausted.
 */
struct zd_client *zd_wm_window_create(struct zd_wm *wm, const char *title,
				      const lv_area_t *geom);

/** Set the titlebar text. */
void zd_wm_window_set_title(struct zd_client *client, const char *title);

/** Move and/or resize. Width and height of 0 leave the current size alone. */
int zd_wm_window_set_geometry(struct zd_client *client, int16_t x, int16_t y, int16_t w,
			      int16_t h);

/**
 * @brief Mark a window for destruction.
 *
 * Never destroys inline. The caller may be a zapp callback running on a stack
 * frame inside the extension's own text, where deleting the LVGL subtree -- let
 * alone unloading the extension -- is a use-after-free. The window is unlinked
 * and queued; zd_wm_reap() does the deleting, from the top of the desktop loop.
 */
void zd_wm_window_close(struct zd_client *client);

/** Destroy everything queued by zd_wm_window_close(). Never call from dispatch. */
void zd_wm_reap(struct zd_wm *wm);

/* --- stacking (stack.c) --- */

/** Rewrite LVGL's child order from wm->stack. */
void zd_wm_restack(struct zd_wm *wm);

/** Move a client to the head of the stack and reproject. */
void zd_wm_raise(struct zd_wm *wm, struct zd_client *client);

/** Topmost client, or NULL if none. */
struct zd_client *zd_wm_top(struct zd_wm *wm);

/* --- focus and dispatch (focus.c) --- */

/** Focus a client, or pass NULL to defocus everything. */
void zd_wm_focus(struct zd_wm *wm, struct zd_client *client);

/** Install the WM's single frame callback and bubble flags. */
void zd_wm_client_attach_events(struct zd_client *client);

/** Install the background's defocus handler. */
void zd_wm_desktop_attach_events(struct zd_wm *wm);

/* --- drag (drag.c) --- */

/** Install titlebar drag handling. */
void zd_wm_drag_attach(struct zd_client *client);

/** Number of live clients. For leak assertions. */
uint32_t zd_wm_client_count(const struct zd_wm *wm);

#endif /* ZD_WM_WM_H_ */
