/*
 * zephyr-desktop — boot-time checks on the things that must refuse.
 *
 * The permission shim and the handle registry are only worth having if they
 * actually say no. Both are easy to break in a way that looks fine: a shim that
 * accepts everything and a registry that dereferences stale handles both make a
 * running desktop that behaves normally right up until it does not.
 *
 * These run at boot and log PASS/FAIL rather than living in a test directory,
 * because the interesting failures are configuration-dependent and would
 * otherwise only be caught on a target nobody runs tests on.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zd/zapp_abi.h>

#include "selftest.h"
#include "chrome/cellgrid.h"
#include "chrome/menu.h"
#include "chrome/rowlist.h"
#include "host/clipboard.h"
#include "host/fs_api.h"
#include "host/balloon_api.h"
#include "host/grid_api.h"
#include "host/list_api.h"
#include "host/timer_api.h"
#include "host/fs_shim.h"
#include "host/storage.h"
#include "host/text_api.h"
#include "loader/zapp_instance.h"
#include "shell/dialog.h"
#include "shell/osk.h"
#include "wm/client.h"
#include "wm/handle.h"
#include "wm/wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

static unsigned int failures;

static void check(bool ok, const char *what)
{
	if (ok) {
		LOG_INF("selftest PASS: %s", what);
	} else {
		LOG_ERR("selftest FAIL: %s", what);
		failures++;
	}
}

static void test_fs_scope(const struct zd_session *session)
{
	char out[ZD_PATH_MAX];
	char probe[ZD_PATH_MAX];

	check(zd_fs_resolve(session, ZD_PATH_TMP "/../../etc/passwd", false, out,
			    sizeof(out)) == -EINVAL,
	      "traversal with .. is refused");

	check(zd_fs_resolve(session, "relative/path", false, out, sizeof(out)) == -EINVAL,
	      "relative path is refused");

	check(zd_fs_resolve(session, "/somewhere/else", false, out, sizeof(out)) == -EACCES,
	      "path outside every root is refused");

	check(zd_fs_resolve(session, ZD_PATH_SYSTEM_ZAPPS "/hello.llext", true, out,
			    sizeof(out)) == -EACCES,
	      "write to the read-only system root is refused");

	check(zd_fs_resolve(session, ZD_PATH_SYSTEM_ZAPPS "/hello.llext", false, out,
			    sizeof(out)) == 0,
	      "read from the system root is allowed");

	/* A root's name must match on a whole component, or "/RAM:/tmpfoo"
	 * would be accepted as living under "/RAM:/tmp".
	 */
	strcpy(probe, ZD_PATH_TMP);
	strcat(probe, "sneaky/file");
	check(zd_fs_resolve(session, probe, true, out, sizeof(out)) == -EACCES,
	      "root prefix match respects component boundaries");

	check(zd_fs_resolve(session, ZD_PATH_TMP "//./scratch", true, out, sizeof(out)) == 0 &&
		      strcmp(out, ZD_PATH_TMP "/scratch") == 0,
	      "duplicate separators and . are normalised away");
}

static void test_handles(void)
{
	int object_a = 1;
	int object_b = 2;
	struct zd_zapp_instance *owner_a = (struct zd_zapp_instance *)0xA;
	struct zd_zapp_instance *owner_b = (struct zd_zapp_instance *)0xB;
	uintptr_t handle;
	uintptr_t reused;

	handle = zd_handle_alloc(ZD_HANDLE_WINDOW, &object_a, owner_a);
	check(handle != 0, "handle allocation succeeds");
	check(zd_handle_deref(handle, ZD_HANDLE_WINDOW, owner_a) == &object_a,
	      "a live handle resolves for its owner");
	check(zd_handle_deref(handle, ZD_HANDLE_WINDOW, owner_b) == NULL,
	      "another app's handle does not resolve");
	check(zd_handle_deref(handle, ZD_HANDLE_LABEL, owner_a) == NULL,
	      "a handle of the wrong kind does not resolve");

	zd_handle_free(handle);
	check(zd_handle_deref(handle, ZD_HANDLE_WINDOW, owner_a) == NULL,
	      "a freed handle does not resolve");

	/* The real hazard: the slot comes back for someone else, and the old
	 * holder's stale copy must still fail rather than reach the new object.
	 */
	reused = zd_handle_alloc(ZD_HANDLE_WINDOW, &object_b, owner_b);
	check(reused != handle, "a reused slot yields a different handle value");
	check(zd_handle_deref(handle, ZD_HANDLE_WINDOW, owner_a) == NULL,
	      "a stale handle does not resolve into the slot's new occupant");
	zd_handle_free(reused);

	check(zd_handle_deref(0, ZD_HANDLE_WINDOW, owner_a) == NULL, "handle 0 is never valid");
}

/*
 * The storage API, driven with a fake owner.
 *
 * fs_api.c never dereferences the owner -- it is a quota and ownership tag, and
 * the session travels separately -- which is precisely what lets these run at
 * boot, before the loader or any instance exists.
 */
static void test_fs_api(const struct zd_session *session)
{
	struct zd_zapp_instance *owner = (struct zd_zapp_instance *)0xF1;
	struct zd_zapp_instance *other = (struct zd_zapp_instance *)0xF2;
	static const char payload[] = "zephyr-desktop storage round trip";
	const char *path = ZD_PATH_TMP "/selftest.txt";
	uint32_t before = zd_fs_open_count();
	char buf[sizeof(payload)] = { 0 };
	uintptr_t handles[CONFIG_ZD_MAX_OPEN_PER_ZAPP + 1] = { 0 };
	struct zd_dirent entry;
	bool quota_hit = false;
	uintptr_t file = 0;
	int ret;

	/* --- the refusals, which are the whole point of the shim --- */

	check(zd_fs_open(session, owner, "/somewhere/else", ZD_O_READ, &file) == -EACCES,
	      "fs_open outside every root is refused");

	check(zd_fs_open(session, owner, ZD_PATH_TMP "/../../etc/passwd", ZD_O_READ, &file) ==
		      -EINVAL,
	      "fs_open with .. is refused");

	check(zd_fs_open(session, owner, ZD_PATH_SYSTEM_ZAPPS "/new.llext",
			 ZD_O_WRITE | ZD_O_CREATE, &file) == -EACCES,
	      "fs_open for write in the read-only system root is refused");

	check(zd_fs_mkdir(session, ZD_PATH_SYSTEM "/nope") == -EACCES,
	      "fs_mkdir in the read-only system root is refused");

	/* --- the round trip --- */

	ret = zd_fs_open(session, owner, path, ZD_O_WRITE | ZD_O_CREATE | ZD_O_TRUNC, &file);
	check(ret == 0, "fs_open creates a file under tmp");
	if (ret != 0) {
		return; /* nothing below can mean anything */
	}

	check(zd_fs_write(owner, file, payload, sizeof(payload)) == (int)sizeof(payload),
	      "fs_write accepts the whole payload");
	zd_fs_close(owner, file);

	check(zd_fs_read(owner, file, buf, sizeof(buf)) == -EBADF,
	      "a file handle used after close returns -EBADF");

	ret = zd_fs_open(session, owner, path, ZD_O_READ, &file);
	check(ret == 0, "fs_open reopens the file for reading");
	check(zd_fs_read(owner, file, buf, sizeof(buf)) == (int)sizeof(payload) &&
		      strcmp(buf, payload) == 0,
	      "fs_read returns exactly what was written");

	/* Kind and ownership are checked on every call, not just on windows. */
	check(zd_fs_readdir(owner, file, &entry) == -EBADF,
	      "a file handle used as a directory does not resolve");
	check(zd_fs_read(other, file, buf, sizeof(buf)) == -EBADF,
	      "another instance's file handle does not resolve");

	zd_fs_close(owner, file);

	/* --- quota and teardown --- */

	for (size_t i = 0; i < ARRAY_SIZE(handles); i++) {
		ret = zd_fs_open(session, owner, path, ZD_O_READ, &handles[i]);
		if (ret != 0) {
			quota_hit = i == CONFIG_ZD_MAX_OPEN_PER_ZAPP && ret == -EMFILE;
			break;
		}
	}

	/* Asserted after the loop, so a quota that never fires is a FAIL rather
	 * than a check that silently never ran.
	 */
	check(quota_hit, "the per-instance open quota is enforced");

	zd_fs_close_all(owner);
	check(zd_fs_open_count() == before,
	      "close_all returns every file the instance left open");

	check(zd_fs_unlink(session, path) == 0, "fs_unlink removes the scratch file");
}

/* Captures the payload of the last on_client_resized() call. */
static struct {
	int16_t w;
	int16_t h;
	unsigned int calls;
} resized;

static void capture_resized(struct zd_client *client, int16_t w, int16_t h)
{
	ARG_UNUSED(client);
	resized.w = w;
	resized.h = h;
	resized.calls++;
}

/* A zapp that was asked and is thinking about it. Stands in for a real one so
 * the deferring half of the handshake can be checked without a loaded
 * extension -- the real hook always answers "nobody to ask" here, because the
 * selftest's windows have no owner.
 */
static bool stub_close_request(struct zd_client *client)
{
	ARG_UNUSED(client);
	return true;
}

/* Do two objects' drawn rectangles share any pixel? */
static bool areas_overlap(lv_obj_t *p, lv_obj_t *q)
{
	lv_area_t pa;
	lv_area_t qa;

	lv_obj_get_coords(p, &pa);
	lv_obj_get_coords(q, &qa);

	return pa.x1 <= qa.x2 && qa.x1 <= pa.x2 && pa.y1 <= qa.y2 && qa.y1 <= pa.y2;
}

/* How many clients are in the stack, mapped or not. */
static uint32_t stacked(struct zd_wm *wm)
{
	struct zd_client *client;
	uint32_t n = 0;

	SYS_DLIST_FOR_EACH_CONTAINER(&wm->stack, client, node) {
		n++;
	}

	return n;
}

/*
 * The window model: stacking, the mapped/unmapped distinction, resize, and the
 * close handshake's escape hatch.
 *
 * Uses owner-less windows -- client->owner == NULL -- which is what a
 * desktop-internal window is, so none of this needs a loader or an extension.
 * That also exercises the "nobody to ask" branch of the close request directly.
 */
static void test_wm(struct zd_wm *wm)
{
	void (*saved_resized)(struct zd_client *, int16_t, int16_t);
	bool (*saved_close)(struct zd_client *);
	uint32_t before = zd_wm_client_count(wm);
	struct zd_client *a;
	struct zd_client *b;
	lv_area_t want = { .x1 = 10, .y1 = 10, .x2 = 10 + 160 - 1, .y2 = 10 + 100 - 1 };

	a = zd_wm_window_create(wm, "selftest A", &want);
	b = zd_wm_window_create(wm, "selftest B", &want);
	if (a == NULL || b == NULL) {
		check(false, "two windows can be created for the WM checks");
		return;
	}

	zd_wm_focus(wm, b);
	check(zd_wm_top(wm) == b, "the newest window is on top");

	/* --- the mapped/unmapped distinction --- */

	zd_wm_window_minimize(b);
	check(zd_wm_top(wm) == a, "zd_wm_top() skips a minimised window");
	check(stacked(wm) == before + 2,
	      "a minimised window keeps its place in the stacking list");
	check(wm->focused == a, "minimising the focused window hands focus on");
	check(lv_obj_has_flag(b->frame, LV_OBJ_FLAG_HIDDEN),
	      "the projection hides a minimised frame");

	zd_wm_window_restore(b);
	check(zd_wm_top(wm) == b && wm->focused == b,
	      "restoring raises and focuses the window");
	check(!lv_obj_has_flag(b->frame, LV_OBJ_FLAG_HIDDEN),
	      "the projection shows a restored frame");

	/*
	 * No two chrome controls may claim the same pixel.
	 *
	 * These are drawn rectangles, which is only the whole story because the
	 * titlebar buttons and the grip deliberately carry no ext_click_area --
	 * their touch allowance is in ZD_BTN_SZ and ZD_GRIP_SZ as real pixels.
	 * That is the point of the check. Hit slop is invisible, LVGL awards an
	 * overlap to whichever object was added last, and the symptom is one
	 * perfectly ordinary-looking button quietly doing another one's job. It
	 * cost the launcher menu its top entry once; with 12 px of slop on two
	 * buttons 2 px apart it would have made every tap on minimise close the
	 * window instead. The value that breaks it lives in a board fragment,
	 * so this is asserted on the target rather than reasoned about here.
	 */
	lv_obj_update_layout(a->frame);
	check(!areas_overlap(a->min_btn, a->close_btn),
	      "the minimise and close buttons do not overlap");

	/*
	 * And that a dialog still fits once the keyboard has taken its half.
	 *
	 * Same category as the overlap checks above and asserted here for the
	 * same reason: every height involved scales with
	 * CONFIG_ZD_TOUCH_SLOP_PX, which lives in a board fragment, and the
	 * screen is whatever the panel is. On the CoreS3 that combination
	 * produced a Save As box 249 px tall on a 240 px display -- negative y,
	 * the filename field behind the keys, and the buttons over the taskbar.
	 * Nothing in the build could have said so; only the target can.
	 */
	check(zd_dialog_fits_with_keyboard(),
	      "a save dialog still fits with the keyboard up");
	check(!areas_overlap(a->min_btn, a->title_label),
	      "the minimise button does not cover the title text");
	check(!areas_overlap(a->grip, a->titlebar),
	      "the resize grip does not reach the titlebar");

	/* --- resize --- */

	saved_resized = wm->on_client_resized;
	wm->on_client_resized = capture_resized;

	zd_wm_window_set_geometry(a, 20, 20, 1, 1);
	check(lv_area_get_width(&a->geom) == ZD_WIN_MIN_W &&
		      lv_area_get_height(&a->geom) == ZD_WIN_MIN_H,
	      "a resize below the minimum is clamped, not applied");

	zd_wm_window_set_geometry(a, 20, 20, 180, 140);
	check(lv_area_get_width(&a->geom) == 180 && lv_area_get_height(&a->geom) == 140,
	      "the model records the requested size");

	/* lv_obj_set_size() only marks the object dirty; the coordinates are not
	 * recomputed until a layout pass, which in the running desktop happens
	 * inside lv_timer_handler(). These checks run before the loop starts, so
	 * force one -- otherwise they measure "has LVGL caught up yet", which is
	 * always no here, rather than "did the projection follow the model".
	 */
	lv_obj_update_layout(a->frame);

	/* The model is only worth anything if LVGL actually followed it. */
	check(lv_obj_get_width(a->frame) == 180 && lv_obj_get_height(a->frame) == 140,
	      "the LVGL projection followed the model's new size");
	check(lv_obj_get_width(a->content) == 180 - 2 * ZD_FRAME_PAD,
	      "the content area was re-laid-out with the frame");

	/*
	 * And the event has to carry the size the window has NOW.
	 *
	 * Worth its own check because the obvious implementation -- read the
	 * content object's width back after setting it -- reports the size the
	 * window used to be, and everything else about the resize still looks
	 * correct. A zapp would lay out for the wrong rectangle with no symptom
	 * anywhere in the WM.
	 */
	check(resized.calls > 0, "a size change delivers on_client_resized()");
	check(resized.w == 180 - 2 * ZD_FRAME_PAD &&
		      resized.h == 140 - 2 * ZD_FRAME_PAD - ZD_TITLEBAR_H - ZD_CONTENT_GAP,
	      "the resize event carries the content area's new size");

	resized.calls = 0;
	zd_wm_window_set_geometry(a, 30, 30, 180, 140);
	check(resized.calls == 0, "a move with no size change delivers no resize event");

	wm->on_client_resized = saved_resized;

	/* --- the close handshake --- */

	/* With somebody to ask, the first request defers and the second one is
	 * the force quit. This is what stops a zapp making itself unclosable by
	 * ignoring the event, without needing any confirm-dialog UI.
	 */
	saved_close = wm->on_client_close_request;
	wm->on_client_close_request = stub_close_request;

	zd_wm_window_close_request(b);
	check(!b->pending_destroy && b->close_requested,
	      "the first close request defers to the zapp");
	zd_wm_window_close_request(b);
	check(b->pending_destroy, "a second close request forces the close");

	/* Declining. The grace period is aimed at a zapp that has stopped
	 * answering, and this is the difference between that and one that says
	 * no -- without it a Cancel button on "save changes?" is decoration.
	 */
	zd_wm_window_close_request(a);
	check(a->close_requested, "a fresh close request is outstanding");
	zd_wm_window_close_cancel(a);
	check(!a->close_requested, "declining clears the request");

	zd_wm_reap(wm);
	check(!a->pending_destroy, "and the window survives its own deadline");

	/* And the conversation starts over rather than force-closing, which is
	 * what would happen if the decline had merely reset a counter.
	 */
	zd_wm_window_close_request(a);
	check(a->close_requested && !a->pending_destroy,
	      "the next request defers again rather than forcing");
	zd_wm_window_close_cancel(a);

	wm->on_client_close_request = saved_close;

	zd_wm_window_close_request(a);
	check(a->pending_destroy,
	      "a close request with no zapp to ask closes immediately");

	zd_wm_reap(wm);
	check(zd_wm_client_count(wm) == before,
	      "the reap returns the client count to where it started");
	check(stacked(wm) == before, "and leaves nothing behind in the stack");
}

/*
 * The text widget: handles, byte positions, selection, and the key routing.
 *
 * Owner-less windows again, with a fabricated instance pointer standing in for
 * a zapp. That is safe here for one specific reason worth stating: a
 * desktop-internal window has handle == 0, and text_api.c refuses to dispatch
 * ZD_EV_TEXT_CHANGED for one -- so the fake owner is never dereferenced.
 */
/*
 * The row list, checked through its own interface rather than the ABI's.
 *
 * The point of nearly every check here is the same one: the model answers
 * immediately and the pixels lag. That is the contract host/list_api.c and the
 * file picker both build on, and it is invisible from the outside -- a list
 * that rebuilt its rows synchronously would pass any test that only looked at
 * what is on screen, and would then use-after-free the first time a zapp
 * entered a directory. So these deliberately never reap.
 */
static void test_rowlist(struct zd_wm *wm)
{
	lv_area_t want = { .x1 = 4, .y1 = 4, .x2 = 4 + 200 - 1, .y2 = 4 + 120 - 1 };
	uint32_t items_before = zd_rowlist_items_used();
	uint32_t lists_before = zd_rowlist_live_count();
	struct zd_client *client;
	struct zd_rowlist *rl;
	char buf[ZD_NAME_MAX];
	int i;

	client = zd_wm_window_create(wm, "selftest list", &want);
	if (client == NULL) {
		check(false, "a window for the list checks could be created");
		return;
	}

	rl = zd_rowlist_create(client->content, 0, 0, 180, 80);
	check(rl != NULL, "a row list can be created");
	if (rl == NULL) {
		zd_wm_window_close(client);
		zd_wm_reap(wm);
		return;
	}

	check(zd_rowlist_count(rl) == 0, "a new list is empty");
	check(zd_rowlist_selected(rl) == -1, "and has nothing selected");
	check(zd_rowlist_capacity() == CONFIG_ZD_LIST_MAX_ITEMS,
	      "capacity is reported, so nobody has to read the Kconfig");

	check(zd_rowlist_add(rl, "alpha", 10) == 0, "the first row is index 0");
	check(zd_rowlist_add(rl, "beta", 11) == 1, "and the second is index 1");
	check(zd_rowlist_add(rl, "gamma", 12) == 2, "and the third is index 2");

	/* The whole design, in one check: three rows exist as far as anyone
	 * asking is concerned, and zd_rowlist_reap() has not run.
	 */
	check(zd_rowlist_count(rl) == 3,
	      "rows count immediately, before any rebuild has happened");
	check(zd_rowlist_item_id(rl, 1) == 11, "a row remembers the id it was given");
	check(zd_rowlist_item_text(rl, 2, buf, sizeof(buf)) == 5 &&
		      strcmp(buf, "gamma") == 0,
	      "and its text, read back out of the model rather than off a label");
	check(zd_rowlist_item_text(rl, 2, buf, 3) == -ENOSPC,
	      "a short buffer is refused, not filled with half a filename");
	check(zd_rowlist_item_id(rl, 3) == -ENOENT, "a row past the end says so");

	check(zd_rowlist_select(rl, 1) == 0 && zd_rowlist_selected(rl) == 1,
	      "the selection round-trips");
	check(zd_rowlist_select(rl, 3) == -EINVAL, "selecting past the end is refused");

	check(zd_rowlist_key(rl, ZD_KEY_DOWN, 0) && zd_rowlist_selected(rl) == 2,
	      "Down moves the selection");
	check(zd_rowlist_key(rl, ZD_KEY_DOWN, 0) && zd_rowlist_selected(rl) == 2,
	      "and stops at the last row rather than wrapping");
	check(zd_rowlist_key(rl, ZD_KEY_HOME, 0) && zd_rowlist_selected(rl) == 0,
	      "Home goes to the top");
	check(zd_rowlist_key(rl, ZD_KEY_END, 0) && zd_rowlist_selected(rl) == 2,
	      "End goes to the bottom");
	check(!zd_rowlist_key(rl, ZD_KEY_CHAR, 0),
	      "a letter is declined, so the zapp still gets it");
	check(!zd_rowlist_key(rl, ZD_KEY_ESCAPE, 0), "and so is Escape");

	zd_rowlist_clear(rl);
	check(zd_rowlist_count(rl) == 0, "clearing empties the list at once");
	check(zd_rowlist_selected(rl) == -1,
	      "and clears the selection, because row 1 of the next contents is "
	      "not what row 1 used to be");
	check(zd_rowlist_items_used() == items_before,
	      "and hands every row back to the shared pool");

	/* Fill it past its own cap. The pool is bigger than one list's share on
	 * purpose, so this proves the per-list bound rather than the pool's.
	 */
	for (i = 0; i < CONFIG_ZD_LIST_MAX_ITEMS; i++) {
		if (zd_rowlist_add(rl, "row", (uint16_t)i) < 0) {
			break;
		}
	}
	check(i == CONFIG_ZD_LIST_MAX_ITEMS, "a list fills to exactly its capacity");
	check(zd_rowlist_add(rl, "one too many", 0) == -ENOSPC,
	      "and then refuses, rather than silently dropping the row");

	check(zd_rowlist_live_count() == lists_before + 1, "one list is live");

	zd_wm_window_close(client);
	zd_wm_reap(wm);

	check(zd_rowlist_live_count() == lists_before,
	      "closing the window releases the list");
	check(zd_rowlist_items_used() == items_before,
	      "and every row it was still holding");
}

/*
 * The list as a zapp sees it: handles, ownership, and the key routing.
 *
 * test_rowlist() already covered the model; what is new here is everything the
 * ABI layer adds, and the part worth having is the ownership check. A zapp
 * holding another zapp's list handle is the failure the registry exists to turn
 * into -EINVAL, and it is invisible until someone tries it.
 */
static void test_list(struct zd_wm *wm)
{
	struct zd_zapp_instance *mine = (struct zd_zapp_instance *)0xb1;
	struct zd_zapp_instance *yours = (struct zd_zapp_instance *)0xb2;
	lv_area_t want = { .x1 = 4, .y1 = 4, .x2 = 4 + 200 - 1, .y2 = 4 + 120 - 1 };
	struct zd_rect geom = { .x = 0, .y = 0, .w = 180, .h = 80 };
	uint32_t before = zd_handle_live_count();
	struct zd_client *client;
	uintptr_t list;
	uintptr_t stale;
	char buf[ZD_NAME_MAX];

	client = zd_wm_window_create(wm, "selftest list api", &want);
	if (client == NULL) {
		check(false, "a window for the list API checks could be created");
		return;
	}

	list = zd_list_create(mine, client, &geom, 0);
	check(list != 0, "a list can be created through the ABI layer");
	check(client->list_focus == NULL,
	      "a new list does not take the keyboard, unlike a text widget");

	check(zd_list_add_item(mine, list, "one", 100) == 0, "a row can be added");
	check(zd_list_add_item(mine, list, "two", 200) == 1, "and another");
	check(zd_list_get_count(mine, list) == 2,
	      "the count answers from the model, with no rebuild in between");
	check(zd_list_get_capacity(mine, list) == CONFIG_ZD_LIST_MAX_ITEMS,
	      "capacity is asked for rather than assumed");

	check(zd_list_get_item_id(mine, list, 1) == 200, "a row's id comes back");
	check(zd_list_get_item_text(mine, list, 0, buf, sizeof(buf)) == 3 &&
		      strcmp(buf, "one") == 0,
	      "and its text");
	check(zd_list_get_item_text(mine, list, 0, buf, 2) == -ENOSPC,
	      "which is refused rather than truncated into a short buffer");

	/* Ownership, the thing the registry is for. */
	check(zd_list_get_count(yours, list) == -EINVAL,
	      "another instance's list handle does not resolve");
	check(zd_list_add_item(yours, list, "sneaky", 0) == -EINVAL,
	      "and cannot be written to either");

	check(zd_list_set_selected(mine, list, 1) == 0 &&
		      zd_list_get_selected(mine, list) == 1,
	      "the selection round-trips through the ABI layer");
	check(zd_list_clear(mine, list) == 0 && zd_list_get_count(mine, list) == 0 &&
		      zd_list_get_selected(mine, list) == -1,
	      "clearing empties and deselects at once, before any reap");

	/* Key routing. The list only gets keys once something has focused it,
	 * which in practice is a click; do it by hand here.
	 */
	zd_list_add_item(mine, list, "a", 1);
	zd_list_add_item(mine, list, "b", 2);
	zd_wm_focus(wm, client);

	check(!zd_list_on_client_key(client, ZD_KEY_DOWN, 0, 0),
	      "a list with no keyboard focus declines the arrow keys");

	zd_list_focus(client, lv_obj_get_child(client->content, 0));
	check(client->list_focus != NULL, "a list can be given the keyboard");
	check(client->text_focus == NULL,
	      "and taking it clears the caret, so one window has one destination");

	zd_wm_key(wm, ZD_KEY_DOWN, 0, 0);
	check(zd_list_get_selected(mine, list) == 0,
	      "Down through the WM's routing reaches the focused list");
	zd_wm_key(wm, ZD_KEY_DOWN, 0, 0);
	check(zd_list_get_selected(mine, list) == 1, "and moves it again");

	/* CTRL must never reach a widget, for the same reason it must never
	 * reach the caret: Ctrl+D is an accelerator, not a cursor movement.
	 */
	zd_wm_key(wm, ZD_KEY_DOWN, 0, ZD_MOD_CTRL);
	check(zd_list_get_selected(mine, list) == 1,
	      "a key held with CTRL does not reach the list");

	check(!zd_list_on_client_key(client, ZD_KEY_CHAR, 'q', 0),
	      "a letter is declined, so a zapp can still use it");

	stale = list;
	zd_list_destroy(mine, list);
	check(zd_list_get_count(mine, stale) == -EINVAL,
	      "a destroyed list's handle stops resolving");

	zd_wm_window_close(client);
	zd_wm_reap(wm);

	check(zd_list_live_count() == 0, "no list widget outlives its window");
	check(zd_handle_live_count() == before, "and no handle does either");
	check(zd_rowlist_items_used() == 0, "and every row is back in the pool");
}

/*
 * The cell grid, through its own interface.
 *
 * Two things here are worth the lines. The first is the same point every list
 * check makes: the model answers at once and the picture lags, so nothing below
 * ever paints. The second is new, and it is the arithmetic -- a grid's hit test
 * is a division, its size is a multiplication, and BOTH are scaled by
 * CONFIG_ZD_TOUCH_SLOP_PX, which lives in a board fragment. A cell size that
 * disagrees with the hit test by one pixel puts every tap near an edge on the
 * neighbouring square, which is unplayable and looks like bad luck.
 *
 * So the border is not hardcoded here: it is derived from what measure() says a
 * 1x1 grid is, and the hit test is then checked against that. The two have to
 * agree with each other rather than with a number written twice.
 */
static void test_cellgrid(struct zd_wm *wm)
{
	lv_area_t want = { .x1 = 4, .y1 = 4, .x2 = 4 + 240 - 1, .y2 = 4 + 200 - 1 };
	uint32_t cells_before = zd_cellgrid_cells_used();
	uint32_t grids_before = zd_cellgrid_live_count();
	int16_t cell = zd_cellgrid_cell_size();
	struct zd_client *client;
	struct zd_cellgrid *g;
	uint8_t col;
	uint8_t row;
	int16_t border;
	int16_t w;
	int16_t h;

	client = zd_wm_window_create(wm, "selftest grid", &want);
	if (client == NULL) {
		check(false, "a window for the grid checks could be created");
		return;
	}

	zd_cellgrid_measure(1, 1, &w, &h);
	border = (int16_t)((w - cell) / 2);
	check(w == h && w == cell + 2 * border, "a one-cell grid is square");

	zd_cellgrid_measure(4, 3, &w, &h);
	check(w == 4 * cell + 2 * border && h == 3 * cell + 2 * border,
	      "and a bigger one is cells plus one border, not one border per cell");

	zd_cellgrid_fit(w, h, &col, &row);
	check(col == 4 && row == 3, "fit inverts measure exactly");

	zd_cellgrid_fit((int16_t)(w - 1), (int16_t)(h - 1), &col, &row);
	check(col == 3 && row == 2, "a pixel short of a cell is a cell short");

	zd_cellgrid_fit(2, 2, &col, &row);
	check(col == 0 && row == 0, "a box too small for one cell fits none");

	g = zd_cellgrid_create(client->content, 0, 0, 5, 4);
	check(g != NULL, "a grid can be created");
	if (g == NULL) {
		zd_wm_window_close(client);
		zd_wm_reap(wm);
		return;
	}

	check(zd_cellgrid_cols(g) == 5 && zd_cellgrid_rows(g) == 4,
	      "and remembers its shape");
	check(zd_cellgrid_capacity() == CONFIG_ZD_GRID_MAX_CELLS,
	      "capacity is reported, so nobody has to read the Kconfig");
	check(zd_cellgrid_cells_used() == cells_before + 20,
	      "and it took exactly cols*rows cells from the shared pool");

	check(zd_cellgrid_style(g, 0, 0) == ZD_CELL_RAISED,
	      "every cell starts raised, which is what an unpressed button is");

	check(zd_cellgrid_set(g, 2, 1, "7", ZD_CELL_SUNKEN, 0x0000FF) == 0,
	      "a cell can be set");
	check(zd_cellgrid_style(g, 2, 1) == ZD_CELL_SUNKEN,
	      "and answers from the model at once, with no repaint in between");
	check(zd_cellgrid_style(g, 2, 2) == ZD_CELL_RAISED, "leaving its neighbour alone");

	check(zd_cellgrid_set(g, 5, 0, "x", ZD_CELL_RAISED, 0) == -EINVAL,
	      "a cell past the last column is refused");
	check(zd_cellgrid_set(g, 0, 4, "x", ZD_CELL_RAISED, 0) == -EINVAL,
	      "and past the last row");
	check(zd_cellgrid_set(g, 0, 0, "x", ZD_CELL_FLAT + 1, 0) == -EINVAL,
	      "and a style that is not one of the three");

	check(zd_cellgrid_clear(g) == 0 && zd_cellgrid_style(g, 2, 1) == ZD_CELL_RAISED,
	      "clearing puts every cell back, immediately");

	/* The hit test, against the border measure() just implied. */
	check(zd_cellgrid_hit(g, border, border, &col, &row) && col == 0 && row == 0,
	      "the top-left pixel inside the border is cell 0,0");
	check(zd_cellgrid_hit(g, (int16_t)(border + cell - 1), border, &col, &row) &&
		      col == 0,
	      "and the last pixel of that cell is still cell 0");
	check(zd_cellgrid_hit(g, (int16_t)(border + cell), border, &col, &row) && col == 1,
	      "and the next one over is cell 1");
	check(zd_cellgrid_hit(g, (int16_t)(border + 4 * cell + cell / 2),
			      (int16_t)(border + 3 * cell + cell / 2), &col, &row) &&
		      col == 4 && row == 3,
	      "the bottom-right cell is where the arithmetic says");
	check(!zd_cellgrid_hit(g, (int16_t)(border - 1), border, &col, &row),
	      "the border itself is not a cell");
	check(!zd_cellgrid_hit(g, (int16_t)(border + 5 * cell), border, &col, &row),
	      "and neither is one column past the end");

	/* Reshaping releases the old block first, so the same space comes back
	 * -- which is the only reason a grid can grow at all once the pool has
	 * anything else in it.
	 */
	check(zd_cellgrid_resize(g, 6, 6) == 0, "a grid can be reshaped");
	check(zd_cellgrid_cols(g) == 6 && zd_cellgrid_rows(g) == 6, "to the new shape");
	check(zd_cellgrid_cells_used() == cells_before + 36, "taking what it now needs");
	/* 255x255 rather than cap+1: the dimensions are bytes, so "one more
	 * than the cap" is not expressible and the honest test is a shape that
	 * is over it by any reckoning.
	 */
	check(zd_cellgrid_resize(g, 255, 255) == -EINVAL,
	      "and refuses to grow past the per-grid cap");
	check(zd_cellgrid_cols(g) == 6,
	      "leaving the shape it had, so the model and the grid still agree");

	check(zd_cellgrid_create(client->content, 0, 0, 255, 255) == NULL,
	      "a grid over the cap is refused rather than clipped");

	check(zd_cellgrid_live_count() == grids_before + 1, "one grid is live");

	zd_wm_window_close(client);
	zd_wm_reap(wm);

	check(zd_cellgrid_live_count() == grids_before,
	      "closing the window releases the grid");
	check(zd_cellgrid_cells_used() == cells_before, "and every cell it was holding");
}

/*
 * The grid as a zapp sees it: handles and ownership.
 *
 * test_cellgrid() covered the model, so what is left is the layer that turns it
 * into an ABI object -- and the check worth having is the same one the list has,
 * because it is the one that is invisible until somebody tries it: a zapp
 * holding another zapp's grid handle gets -EINVAL and not a board.
 */
static void test_grid(struct zd_wm *wm)
{
	struct zd_zapp_instance *mine = (struct zd_zapp_instance *)0xc1;
	struct zd_zapp_instance *yours = (struct zd_zapp_instance *)0xc2;
	lv_area_t want = { .x1 = 4, .y1 = 4, .x2 = 4 + 240 - 1, .y2 = 4 + 200 - 1 };
	uint32_t before = zd_handle_live_count();
	struct zd_client *client;
	uintptr_t grid;
	uintptr_t stale;

	client = zd_wm_window_create(wm, "selftest grid api", &want);
	if (client == NULL) {
		check(false, "a window for the grid API checks could be created");
		return;
	}

	grid = zd_grid_create(mine, client, 0, 0, 4, 4);
	check(grid != 0, "a grid can be created through the ABI layer");

	check(zd_grid_set_cell(mine, grid, 1, 1, "*", ZD_CELL_SUNKEN, 0xFF0000) == 0,
	      "a cell can be set through a handle");
	check(zd_grid_set_cell(mine, grid, 9, 9, "*", ZD_CELL_SUNKEN, 0) == -EINVAL,
	      "and one off the board still cannot");

	check(zd_grid_set_cell(yours, grid, 0, 0, "!", ZD_CELL_RAISED, 0) == -EINVAL,
	      "another instance's grid handle does not resolve");
	check(zd_grid_clear(yours, grid) == -EINVAL, "and cannot be cleared either");
	check(zd_grid_resize(yours, grid, 2, 2) == -EINVAL, "or reshaped");

	check(zd_grid_resize(mine, grid, 3, 7) == 0, "the owner can reshape it");
	check(zd_grid_clear(mine, grid) == 0, "and clear it");
	check(zd_grid_set_pos(mine, grid, 10, 20) == 0, "and move it");

	check(zd_grid_live_count() == 1, "one grid widget is live");

	stale = grid;
	zd_grid_destroy(mine, grid);
	check(zd_grid_clear(mine, stale) == -EINVAL,
	      "a destroyed grid's handle stops resolving");

	zd_wm_window_close(client);
	zd_wm_reap(wm);

	check(zd_grid_live_count() == 0, "no grid widget outlives its window");
	check(zd_handle_live_count() == before, "and no handle does either");
	check(zd_cellgrid_cells_used() == 0, "and every cell is back in the pool");
}

/*
 * Balloons: the ownership check, and the one thing that is genuinely new.
 *
 * Most of this mirrors test_grid() -- create, drive through a handle, prove
 * another instance's handle does not resolve, prove nothing outlives the
 * window. The part worth having is the icon validation, because ZD_ICON_* and
 * ZD_ICON_STATE_* are the first enums a zapp passes across the ABI as bare
 * uint32_t. An out-of-range value there has to be refused rather than indexed
 * with.
 */
static void test_balloon(struct zd_wm *wm)
{
	struct zd_zapp_instance *mine = (struct zd_zapp_instance *)0xb1;
	struct zd_zapp_instance *yours = (struct zd_zapp_instance *)0xb2;
	lv_area_t want = { .x1 = 4, .y1 = 4, .x2 = 4 + 300 - 1, .y2 = 4 + 200 - 1 };
	uint32_t before = zd_handle_live_count();
	struct zd_client *client;
	uintptr_t balloon;
	uintptr_t stale;

	client = zd_wm_window_create(wm, "selftest balloon api", &want);
	if (client == NULL) {
		check(false, "a window for the balloon API checks could be created");
		return;
	}

	balloon = zd_balloon_api_create(mine, client, 0, 0, 280, 60, ZD_ICON_CLIP);
	check(balloon != 0, "a balloon can be created through the ABI layer");

	check(zd_balloon_api_create(mine, client, 0, 0, 280, 60, 99) == 0,
	      "an icon outside the enum is refused rather than drawn");

	check(zd_balloon_api_set_text(mine, balloon, "hello") == 0,
	      "the owner can set the text");
	check(zd_balloon_api_set_icon(mine, balloon, ZD_ICON_CLIP,
				      ZD_ICON_STATE_BUSY) == 0,
	      "and the expression");
	check(zd_balloon_api_set_icon(mine, balloon, ZD_ICON_CLIP, 99) == -EINVAL,
	      "an icon state outside the enum is refused");
	check(zd_balloon_api_set_icon(mine, balloon, 99, ZD_ICON_STATE_NORMAL) == -EINVAL,
	      "and so is an icon outside it");
	check(zd_balloon_api_set_geometry(mine, balloon, 2, 2, 200, 40) == 0,
	      "the owner can move and resize it");
	check(zd_balloon_api_set_geometry(mine, balloon, 2, 2, 0, 40) == -EINVAL,
	      "a zero-width balloon is refused");

	check(zd_balloon_api_set_text(yours, balloon, "mine now") == -EINVAL,
	      "another instance's balloon handle does not resolve");
	check(zd_balloon_api_set_icon(yours, balloon, ZD_ICON_WARN, 0) == -EINVAL,
	      "and its icon cannot be changed either");

	check(zd_balloon_api_live_count() == 1, "one balloon widget is live");

	stale = balloon;
	zd_balloon_api_destroy(mine, balloon);
	check(zd_balloon_api_set_text(mine, stale, "still there?") == -EINVAL,
	      "a destroyed balloon's handle stops resolving");

	zd_wm_window_close(client);
	zd_wm_reap(wm);

	check(zd_balloon_api_live_count() == 0, "no balloon widget outlives its window");
	check(zd_handle_live_count() == before, "and no handle does either");
}

/*
 * Timers, without ever letting one fire.
 *
 * lv_timer callbacks only run from lv_timer_handler(), which the desktop loop
 * has not started yet -- so the fabricated owner pointers below are never
 * dereferenced, the same trick and the same reason as the widget checks above.
 * What is checked is the bookkeeping, which is where the bugs would be: a
 * re-arm that quietly makes a second timer, a stop that leaves the slot taken,
 * or an instance teardown that leaves one running into code that is about to be
 * unmapped.
 */
static void test_timer(void)
{
	struct zd_zapp_instance *mine = (struct zd_zapp_instance *)0xd1;
	struct zd_zapp_instance *yours = (struct zd_zapp_instance *)0xd2;
	uint32_t before = zd_timer_live_count();
	int spare;

	check(zd_timer_start(NULL, 1000, 1) == -EINVAL, "a timer needs an owner");

	check(zd_timer_start(mine, 1000, 1) == 0, "a timer can be started");
	check(zd_timer_live_count() == before + 1, "and is counted");

	check(zd_timer_start(mine, 250, 1) == 0, "starting the same id again succeeds");
	check(zd_timer_live_count() == before + 1,
	      "and re-arms the one that is there rather than making a second");

	check(zd_timer_start(mine, 1000, 2) == 0, "a second id is a second timer");
	check(zd_timer_live_count() == before + 2, "and is counted separately");

	check(zd_timer_start(yours, 1000, 1) == 0,
	      "another instance may use the same id, because ids are per instance");
	check(zd_timer_live_count() == before + 3, "and gets its own slot");

	/* Fill whatever is left, then prove the wall says so rather than
	 * silently handing back a slot somebody else is using.
	 */
	spare = CONFIG_ZD_MAX_TIMERS - (int)zd_timer_live_count();
	for (int i = 0; i < spare; i++) {
		zd_timer_start(mine, 1000, (uint16_t)(100 + i));
	}
	check(zd_timer_live_count() == CONFIG_ZD_MAX_TIMERS, "the table fills");
	check(zd_timer_start(mine, 1000, 999) == -ENOSPC, "and then refuses");

	check(zd_timer_stop(mine, 999) == 0,
	      "stopping one that is not running is not an error");
	check(zd_timer_live_count() == CONFIG_ZD_MAX_TIMERS, "and changes nothing");

	check(zd_timer_stop(mine, 2) == 0 &&
		      zd_timer_live_count() == CONFIG_ZD_MAX_TIMERS - 1,
	      "stopping a running one frees its slot");

	zd_timer_owner_gone(mine);
	check(zd_timer_live_count() == 1,
	      "an instance going takes every timer it had, and none that it did not");

	zd_timer_owner_gone(yours);
	check(zd_timer_live_count() == 0, "and the other instance's too");
}

/*
 * What zapp_launch() refuses.
 *
 * Only the refusals: a successful request queues a real load, which would then
 * happen at the first reap and put a window on a desktop that is still booting.
 * The refusals are also the interesting half -- they are the whole reason the
 * call validates synchronously instead of logging a frame later.
 */
static void test_launch_request(void)
{
	char toolong[ZD_PATH_MAX + 8];

	memset(toolong, 'x', sizeof(toolong) - 1);
	toolong[sizeof(toolong) - 1] = '\0';

	check(zd_zapp_launch_request(NULL, NULL) == -EINVAL, "launching nothing is refused");
	check(zd_zapp_launch_request("", NULL) == -EINVAL,
	      "and so is launching the empty name");
	check(zd_zapp_launch_request("no-such-zapp", NULL) == -ENOENT,
	      "a name that was never discovered comes back as -ENOENT, at once");
	check(zd_zapp_launch_request("notepad", toolong) == -EINVAL,
	      "an argument too long to hold is refused rather than truncated");
}

static void test_text(struct zd_wm *wm)
{
	struct zd_zapp_instance *mine = (struct zd_zapp_instance *)0xa1;
	struct zd_client *client;
	lv_area_t want = { .x1 = 4, .y1 = 4, .x2 = 4 + 200 - 1, .y2 = 4 + 120 - 1 };
	struct zd_rect geom = { .x = 0, .y = 0, .w = 180, .h = 80 };
	uint32_t before = zd_handle_live_count();
	uintptr_t text;
	char buf[32];
	uint32_t from;
	uint32_t to;

	client = zd_wm_window_create(wm, "selftest text", &want);
	if (client == NULL) {
		check(false, "a window for the text checks could be created");
		return;
	}

	text = zd_text_create(mine, client, &geom, 0);
	check(text != 0, "a text widget can be created");
	check(client->text_focus != NULL,
	      "the first editable widget in a window takes the caret");

	check(zd_text_set_text(mine, text, "hello world") == 0, "set_text succeeds");
	check(zd_text_get_length(mine, text) == 11, "get_length counts bytes");

	check(zd_text_get_text(mine, text, 6, buf, sizeof(buf)) == 5 &&
		      strcmp(buf, "world") == 0,
	      "get_text reads from a byte offset");
	check(zd_text_get_text(mine, text, 11, buf, sizeof(buf)) == 0,
	      "get_text at the end returns 0, not an error");
	check(zd_text_get_text(mine, text, 0, buf, 4) == 3 && strcmp(buf, "hel") == 0,
	      "get_text is allowed to be short and terminates what it wrote");

	check(zd_text_set_cursor(mine, text, 5) == 0 &&
		      zd_text_get_cursor(mine, text) == 5,
	      "the caret round-trips through a byte offset");

	check(zd_text_get_selection(mine, text, &from, &to) == 0,
	      "nothing is selected to begin with");
	check(zd_text_select(mine, text, 0, 5) == 0 &&
		      zd_text_get_selection(mine, text, &from, &to) == 1 && from == 0 &&
		      to == 5,
	      "a selection round-trips through byte offsets");

	check(zd_text_delete_selection(mine, text) == 1 &&
		      zd_text_get_length(mine, text) == 6,
	      "deleting the selection removes exactly it");
	check(zd_text_delete_selection(mine, text) == 0,
	      "deleting nothing reports that it deleted nothing");

	/* Keys reach the widget only through the WM's routing, so drive it the
	 * way a real key press does rather than calling the widget directly.
	 */
	zd_wm_focus(wm, client);
	zd_text_set_cursor(mine, text, 0);
	zd_wm_key(wm, ZD_KEY_CHAR, 'X', 0);
	check(zd_text_get_length(mine, text) == 7, "a typed character reaches the widget");

	check(!zd_text_on_client_key(client, ZD_KEY_ESCAPE, 0, 0),
	      "Escape is declined, so the zapp gets it");
	check(!zd_text_on_client_key(client, ZD_KEY_F(1), 0, 0),
	      "a function key is declined too");

	zd_text_select(mine, text, 0, 3);
	zd_wm_key(wm, ZD_KEY_CHAR, 'Q', 0);
	check(zd_text_get_length(mine, text) == 5,
	      "typing over a selection replaces it");

	/* CTRL must never reach the caret, or Ctrl+S types an S. */
	{
		int len = zd_text_get_length(mine, text);

		zd_wm_key(wm, ZD_KEY_CHAR, 's', ZD_MOD_CTRL);
		check(zd_text_get_length(mine, text) == len,
		      "a key held with CTRL does not reach the text widget");
	}

	/* --- the clipboard, which only means anything against a widget --- */

	check(zd_text_set_text(mine, text, "alpha beta") == 0, "set_text for the copy");
	check(zd_text_copy(mine, text) == 0, "copying nothing is a no-op, not an error");

	zd_text_select(mine, text, 0, 5);
	check(zd_text_copy(mine, text) == 5 && zd_clipboard_length() == 5,
	      "copy stores exactly the selection");
	check(zd_clipboard_get(0, buf, sizeof(buf)) == 5 && strcmp(buf, "alpha") == 0,
	      "and the clipboard hands it back");

	zd_text_select(mine, text, 6, 10);
	check(zd_text_cut(mine, text) == 4 && zd_text_get_length(mine, text) == 6,
	      "cut removes what it copied");
	check(zd_clipboard_length() == 4, "and the clipboard now holds it");

	zd_text_set_cursor(mine, text, 6);
	check(zd_text_paste(mine, text) == 4 && zd_text_get_length(mine, text) == 10,
	      "paste puts it back");

	check(zd_clipboard_set("x", 1) == 1 && zd_clipboard_get(0, buf, 1) == 0,
	      "clipboard_get into a one-byte buffer returns 0, not an overflow");

	{
		static char big[CONFIG_ZD_CLIPBOARD_MAX + 64];

		memset(big, 'z', sizeof(big));
		check(zd_clipboard_set(big, sizeof(big)) == CONFIG_ZD_CLIPBOARD_MAX,
		      "an oversized copy truncates rather than failing");
	}

	check(zd_text_live_count() == 1, "one text widget is live");

	/* Closing the window must take the widget and its handle with it: the
	 * only cleanup path is LVGL's DELETE event, so this is the check that
	 * the path is actually wired.
	 */
	zd_wm_window_close(client);
	zd_wm_reap(wm);
	check(zd_text_live_count() == 0, "closing the window destroys its text widget");
	check(zd_handle_live_count() == before,
	      "and releases every handle the window held");
}

/*
 * Menu bars: the geometry, the bookkeeping, and the thing that has bitten this
 * project three times.
 *
 * The overlap check runs on the target rather than in a test directory for the
 * usual reason -- the value that breaks it, CONFIG_ZD_TOUCH_SLOP_PX, lives in a
 * board fragment, so a desktop that is fine on QEMU and unusable by thumb is
 * exactly the failure this catches.
 */
static void test_menu(struct zd_wm *wm)
{
	struct zd_zapp_instance *mine = (struct zd_zapp_instance *)0xa2;
	struct zd_client *client;
	lv_area_t want = { .x1 = 4, .y1 = 4, .x2 = 4 + 220 - 1, .y2 = 4 + 140 - 1 };
	uint32_t before = zd_handle_live_count();
	uintptr_t bar;
	uintptr_t file;
	uintptr_t edit;
	lv_area_t a;
	lv_area_t b;
	int16_t w0;
	int16_t h0;
	int16_t w1;
	int16_t h1;

	client = zd_wm_window_create(wm, "selftest menu", &want);
	if (client == NULL) {
		check(false, "a window for the menu checks could be created");
		return;
	}

	zd_client_content_size(client, &w0, &h0);

	bar = zd_menubar_create(mine, client);
	check(bar != 0, "a window can be given a menu bar");
	check(zd_menubar_create(mine, client) == 0, "but only one");

	zd_client_content_size(client, &w1, &h1);
	check(w1 == w0 && h1 == h0 - ZD_MENUBAR_H,
	      "the content area gives up exactly the bar's height");

	file = zd_menu_add_submenu(mine, bar, "File");
	edit = zd_menu_add_submenu(mine, bar, "Edit");
	check(file != 0 && edit != 0, "drop-downs can be added to the bar");
	check(file != edit, "and are distinct");

	lv_obj_update_layout(client->frame);
	check(zd_menu_title_coords(bar, 0, &a) && zd_menu_title_coords(bar, 1, &b),
	      "the bar reports its title rectangles");
	check(a.x2 < b.x1, "adjacent menu titles do not overlap");
	/*
	 * And that they are actually ADJACENT, which is the half this check was
	 * missing and which cost milestone L an afternoon.
	 *
	 * A title's x came from lv_obj_get_width() of the one before it, read
	 * before any layout pass, so it answered LVGL's default 130 px instead
	 * of the ~32 px the title really is. Every bar was spaced by 130: a dead
	 * gap between File and Edit that looked like a wide menu bar, and on a
	 * window narrower than 260 px a second title placed off the end of the
	 * bar entirely, where it cannot be clicked. Non-overlap passed happily
	 * throughout -- two titles a hundred pixels apart do not overlap.
	 *
	 * A gap of exactly zero is the assertion, because "titles sit edge to
	 * edge" is what the sizing comment in chrome/menu.c claims and the only
	 * version of it that is checkable.
	 */
	check(b.x1 == a.x2 + 1,
	      "and sit edge to edge, so the bar is spaced by its titles and not "
	      "by an LVGL default");
	check(b.x2 - client->geom.x1 < lv_area_get_width(&client->geom),
	      "and the last title is still inside the window");
	check(lv_area_get_height(&a) >= ZD_MENUBAR_H - 1,
	      "a menu title is as tall as the bar");

	check(zd_menu_add_item(mine, file, "Open", 1) == 0, "an item can be added");
	check(zd_menu_add_separator(mine, file) == 0, "and a separator");
	check(zd_menu_add_item(mine, file, "Exit", 2) == 0, "and another item");
	check(zd_menu_add_item(mine, bar, "wrong", 3) == -EINVAL,
	      "items cannot be added to the bar itself");

	check(zd_menu_set_item_enabled(mine, file, 2, false) == 0,
	      "an item can be disabled by id");
	check(zd_menu_set_item_enabled(mine, file, 99, false) == -ENOENT,
	      "and an id that is not there says so");

	while (zd_menu_add_item(mine, edit, "x", 0) == 0) {
		/* fill it */
	}
	check(zd_menu_add_item(mine, edit, "x", 0) == -EINVAL,
	      "a full drop-down refuses more items");

	check(zd_menu_live_count() == 3, "one bar and two drop-downs are live");

	zd_wm_window_close(client);
	zd_wm_reap(wm);
	check(zd_menu_live_count() == 0,
	      "closing the window releases the bar and every drop-down on it");
	check(zd_handle_live_count() == before, "and every handle they held");
}

void zd_selftest_run(const struct zd_session *session)
{
	failures = 0;

	test_fs_scope(session);
	test_handles();
	test_fs_api(session);

	if (failures == 0) {
		LOG_INF("selftest: all checks passed");
	} else {
		LOG_ERR("selftest: %u check(s) FAILED", failures);
	}
}

void zd_selftest_run_wm(struct zd_wm *wm)
{
	/*
	 * THE CHECKS MUST LEAVE THE DESKTOP AS THEY FOUND IT, and until
	 * milestone M they did not.
	 *
	 * test_text() creates a text widget, a new text widget takes the caret,
	 * and taking the caret calls zd_osk_wanted(true) -- which on a board
	 * with CONFIG_ZD_OSK_AUTO raises the on-screen keyboard and, by design,
	 * never lowers it again. So every boot on the CoreS3 came up with the
	 * keyboard across the bottom half of a 240 px screen, put there by a
	 * test, and the taskbar toggle was the only way to find out.
	 *
	 * It survived a whole milestone because QEMU has slop 0 and therefore
	 * no OSK_AUTO, so the desktop this is developed on cannot show it. What
	 * found it was building the QEMU image with the CoreS3's panel size and
	 * touch slop -- see docs/hardware.md -- and then wondering why a click
	 * on a Minesweeper cell arrived as the letter 'd'.
	 */
	bool osk_was = zd_osk_visible();

	failures = 0;

	test_wm(wm);
	test_rowlist(wm);
	test_list(wm);
	test_cellgrid(wm);
	test_grid(wm);
	test_balloon(wm);
	test_timer();
	test_launch_request();
	test_text(wm);
	test_menu(wm);

	if (zd_osk_visible() != osk_was) {
		zd_osk_set_visible(osk_was);
	}
	check(zd_osk_visible() == osk_was,
	      "the checks left the on-screen keyboard as they found it");

	if (failures == 0) {
		LOG_INF("selftest (wm): all checks passed");
	} else {
		LOG_ERR("selftest (wm): %u check(s) FAILED", failures);
	}
}
