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

#include <zd/zapp_abi.h>

#include "../shell/desktop.h"

/* ZD_TITLE_MAX comes from the ABI: a zapp has to be able to build a title, so
 * the bound belongs in the contract rather than being the WM's private secret.
 */

/*
 * Chrome geometry, in pixels.
 *
 * The controls grow by CONFIG_ZD_TOUCH_SLOP_PX rather than getting that much
 * invisible hit area, and the titlebar grows with them. This is the launcher
 * menu's lesson -- see the comment on ITEM_H in shell/launcher.c -- applied to
 * the second place it bites: two titlebar buttons two pixels apart, each with
 * 12 px of ext_click_area on a touch board, overlap completely, and LVGL awards
 * an overlap to the last child. Every tap on minimise would have closed the
 * window instead. Adjacent controls need to be bigger, not to claim more space
 * than they occupy.
 *
 * Where the slop is 0 -- QEMU, and anything with a mouse -- these are the
 * original numbers and the desktop looks exactly as it did.
 */
#define ZD_FRAME_PAD   3 /**< frame edge to titlebar/content */
#define ZD_BTN_SZ      (14 + CONFIG_ZD_TOUCH_SLOP_PX) /**< a titlebar button */
#define ZD_TITLEBAR_H  (ZD_BTN_SZ + 4)
#define ZD_CONTENT_GAP 2 /**< titlebar to content */
#define ZD_GRIP_SZ     (12 + CONFIG_ZD_TOUCH_SLOP_PX) /**< resize grip, bottom-right */
/** Menu bar, when a window has one. Titles sit edge to edge, so this grows. */
#define ZD_MENUBAR_H   (14 + CONFIG_ZD_TOUCH_SLOP_PX)

/* Wide enough for both buttons plus something of a title. */
#define ZD_WIN_MIN_W (ZD_FRAME_PAD * 2 + 2 * ZD_BTN_SZ + 40)
#define ZD_WIN_MIN_H (ZD_FRAME_PAD * 2 + ZD_TITLEBAR_H + ZD_CONTENT_GAP + 20)

/* The relationships a board fragment could break by raising the touch slop. */
BUILD_ASSERT(ZD_TITLEBAR_H >= ZD_BTN_SZ, "the titlebar cannot hold its own buttons");
BUILD_ASSERT(ZD_WIN_MIN_W - 2 * ZD_FRAME_PAD - 2 * ZD_BTN_SZ >= 20,
	     "the smallest window has no room left for a title");

struct zd_zapp_instance; /* milestone F */
struct zd_wm;

/**
 * What a titlebar or grip press is currently doing.
 *
 * One shape of handler drives both -- press, pressing, release -- so the mode
 * is what tells them apart, rather than two near-identical state machines.
 */
enum zd_drag_mode {
	ZD_DRAG_NONE,
	ZD_DRAG_MOVE,
	ZD_DRAG_RESIZE,
};

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
	lv_obj_t *min_btn;
	lv_obj_t *close_btn;
	lv_obj_t *content; /**< the app's area */
	lv_obj_t *grip;    /**< resize handle */

	/**
	 * Menu bar, or NULL. Created on demand by chrome/menu.c, owned by the
	 * subtree, and subtracted from the content area -- which is why the
	 * height is cached here rather than read back off the object: it is
	 * needed by zd_client_content_size(), which is the one place the model
	 * is allowed to tell LVGL what the geometry is.
	 */
	lv_obj_t *menubar;
	int16_t menubar_h;

	/**
	 * The text widget with the caret in this window, or NULL.
	 *
	 * Keyboard focus *within* a window, which is the same kind of state as
	 * focus between windows and so lives in the same struct. wm/keys.c
	 * consults it; host/text_api.c sets it. The WM does not know what an
	 * lv_textarea is and does not need to -- it only ever asks the hook
	 * whether something consumed the key.
	 */
	lv_obj_t *text_focus;

	/* WM-authoritative geometry. LVGL follows this, never the reverse. */
	lv_area_t geom;
	char title[ZD_TITLE_MAX];

	bool focused;
	bool pending_destroy;

	/**
	 * Unmapped, but still alive and still in wm->stack.
	 *
	 * The X11 distinction, and the reason minimise is nearly free here:
	 * z-order survives it untouched, because the list never changed. Only
	 * the projection does -- zd_wm_restack() hides the frame rather than
	 * placing it. Anything walking the stack for a *visible* window has to
	 * skip these; zd_wm_top() does.
	 */
	bool minimized;

	/* Close handshake. The zapp has been asked and has not yet answered;
	 * after the deadline the desktop stops asking. See
	 * zd_wm_window_close_request().
	 */
	bool close_requested;
	int64_t close_deadline;

	struct zd_zapp_instance *owner; /**< NULL == desktop-internal window */
	uintptr_t handle;              /**< the app's handle for this window, or 0 */

	/* Drag state, valid only while dragging. */
	lv_point_t drag_grab;   /**< pointer position at press */
	lv_point_t drag_origin; /**< window position at press */
	lv_point_t drag_size;   /**< window size at press, for ZD_DRAG_RESIZE */
	enum zd_drag_mode drag_mode;

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

	/* Called on a click inside a client's content area, with coordinates
	 * relative to that area's top-left -- the same origin a zapp positions
	 * its widgets in, so it never has to know the chrome's dimensions.
	 */
	void (*on_client_click)(struct zd_client *client, int16_t x, int16_t y);

	/* Called when a client's content area finished changing size. Fired on
	 * release rather than per pointer sample; see ZD_EV_RESIZED.
	 */
	void (*on_client_resized)(struct zd_client *client, int16_t w, int16_t h);

	/* Called when a client was minimised or restored. */
	void (*on_client_minimized)(struct zd_client *client, bool minimized);

	/* Called with a key press for the focused client, once the routing in
	 * wm/keys.c has decided the zapp is the right recipient.
	 */
	void (*on_client_key)(struct zd_client *client, uint32_t code, uint32_t unicode,
			      uint16_t mods);

	/* Offered every key press before anything else, so a modal surface can
	 * take the keyboard. Returning true means "consumed"; nothing else
	 * sees it, including the focused window. This is what modal means for
	 * a keyboard, and it is the only hook that fires with no client at all.
	 */
	bool (*on_key_grab)(uint32_t code, uint32_t unicode, uint16_t mods);

	/* Offered a key press BEFORE on_client_key, so a focused text widget
	 * can swallow ordinary typing. Returning true means "consumed"; the
	 * zapp is then not told, exactly as it is not told about the pointer
	 * samples during a titlebar drag.
	 *
	 * Never called with CTRL held: an accelerator must beat the caret.
	 */
	bool (*on_client_text_key)(struct zd_client *client, uint32_t code, uint32_t unicode,
				   uint16_t mods);

	/* Called when the user asks to close a window, so the owner can ask the
	 * zapp first. Returning false means "nobody could answer, close it now".
	 */
	bool (*on_client_close_request)(struct zd_client *client);

	/* Asked when a close request's grace period runs out, just before the
	 * window is taken anyway. Returning true means the zapp is visibly
	 * asking the user rather than ignoring the desktop, and buys it
	 * another grace period.
	 */
	bool (*on_client_close_stalled)(struct zd_client *client);

	/* Called whenever the set of windows, or anything the shell displays
	 * about them, changed: created, closed, retitled, minimised, focused.
	 *
	 * Unlike the hooks above this one is about the whole desktop rather than
	 * one client, and it is the first thing the WM tells the shell rather
	 * than the loader. It MUST NOT rebuild anything inline -- see the note
	 * on zd_tasklist_invalidate().
	 */
	void (*on_client_list_changed)(struct zd_wm *wm);
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

/**
 * @brief Move and/or resize.
 *
 * Width and height of 0 leave the current size alone; anything below the window
 * minimum is raised to it. A size change fires on_client_resized().
 */
int zd_wm_window_set_geometry(struct zd_client *client, int16_t x, int16_t y, int16_t w,
			      int16_t h);

/**
 * @brief Unmap a window without destroying it.
 *
 * It keeps its handle, its widgets and its place in the stack; it simply stops
 * drawing and stops being clickable. Focus moves to whatever is now topmost.
 */
void zd_wm_window_minimize(struct zd_client *client);

/** Map a minimised window again, raise it and focus it. */
void zd_wm_window_restore(struct zd_client *client);

/**
 * @brief Ask for a window to be closed, on the user's behalf.
 *
 * This is what the close box and the taskbar go through, and it is the polite
 * form of zd_wm_window_close(): the owning zapp gets a ZD_EV_WINDOW_CLOSE_REQUEST
 * and a bounded grace period in which to flush and close itself.
 *
 * A window with no owner, a second request while one is outstanding, or a grace
 * period that expires all close immediately: a zapp cannot keep its window by
 * IGNORING the event. It can keep it by answering -- see
 * zd_wm_window_close_cancel().
 */
void zd_wm_window_close_request(struct zd_client *client);

/**
 * @brief The owning zapp has declined this close.
 *
 * Clears the outstanding request and its deadline, so the window stays and the
 * user's next click starts the conversation over from the beginning.
 *
 * This is the difference between "ask" and "wait 2000 ms and take it anyway",
 * and without it a Cancel button on a "save changes?" box is decoration. The
 * grace period is aimed at a zapp that has stopped answering; a zapp that says
 * no is answering. What that costs is that a zapp determined to keep its window
 * can -- which is the same bargain every desktop makes, and the reason they all
 * have an end-task of some kind. This one does not yet.
 */
void zd_wm_window_close_cancel(struct zd_client *client);

/**
 * @brief Mark a window for destruction.
 *
 * Never destroys inline. The caller may be a zapp callback running on a stack
 * frame inside the extension's own text, where deleting the LVGL subtree -- let
 * alone unloading the extension -- is a use-after-free. The window is unlinked
 * and queued; zd_wm_reap() does the deleting, from the top of the desktop loop.
 */
void zd_wm_window_close(struct zd_client *client);

/**
 * @brief Destroy everything queued by zd_wm_window_close().
 *
 * Also sweeps expired close requests, so a zapp that ignores
 * ZD_EV_WINDOW_CLOSE_REQUEST still loses its window. Never call from dispatch.
 */
void zd_wm_reap(struct zd_wm *wm);

/** Fire on_client_list_changed(), if anything is listening. */
void zd_wm_notify_list_changed(struct zd_wm *wm);

/** Fire on_client_resized() with the content area's current size. */
void zd_wm_notify_resized(struct zd_client *client);

/* --- stacking (stack.c) --- */

/** Rewrite LVGL's child order from wm->stack. */
void zd_wm_restack(struct zd_wm *wm);

/** Move a client to the head of the stack and reproject. */
void zd_wm_raise(struct zd_wm *wm, struct zd_client *client);

/**
 * @brief Topmost *mapped* client, or NULL if none.
 *
 * Minimised clients are skipped. They are still in wm->stack -- walk it
 * directly if you want every client rather than every visible one.
 */
struct zd_client *zd_wm_top(struct zd_wm *wm);

/* --- keys (keys.c) --- */

/**
 * @brief Route one key press.
 *
 * Called from the single input funnel in input/keys.h, whichever source it came
 * from. The policy is documented at the top of wm/keys.c.
 */
void zd_wm_key(struct zd_wm *wm, uint32_t code, uint32_t unicode, uint16_t mods);

/* --- focus and dispatch (focus.c) --- */

/** Focus a client, or pass NULL to defocus everything. */
void zd_wm_focus(struct zd_wm *wm, struct zd_client *client);

/** Install the WM's single frame callback and bubble flags. */
void zd_wm_client_attach_events(struct zd_client *client);

/** Install the background's defocus handler. */
void zd_wm_desktop_attach_events(struct zd_wm *wm);

/* --- drag (drag.c) --- */

/** Install titlebar drag-to-move and grip drag-to-resize handling. */
void zd_wm_drag_attach(struct zd_client *client);

/** Number of live clients. For leak assertions. */
uint32_t zd_wm_client_count(const struct zd_wm *wm);

#endif /* ZD_WM_WM_H_ */
