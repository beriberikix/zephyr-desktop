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

#include "selftest.h"
#include "host/fs_api.h"
#include "host/fs_shim.h"
#include "host/storage.h"
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

	wm->on_client_close_request = saved_close;

	zd_wm_window_close_request(a);
	check(a->pending_destroy,
	      "a close request with no zapp to ask closes immediately");

	zd_wm_reap(wm);
	check(zd_wm_client_count(wm) == before,
	      "the reap returns the client count to where it started");
	check(stacked(wm) == before, "and leaves nothing behind in the stack");
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
	failures = 0;

	test_wm(wm);

	if (failures == 0) {
		LOG_INF("selftest (wm): all checks passed");
	} else {
		LOG_ERR("selftest (wm): %u check(s) FAILED", failures);
	}
}
