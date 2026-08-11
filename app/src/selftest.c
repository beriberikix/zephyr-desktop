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
#include "host/fs_shim.h"
#include "host/storage.h"
#include "wm/handle.h"

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

	check(zd_fs_resolve(session, ZD_PATH_SYSTEM_APPS "/hello.llext", true, out,
			    sizeof(out)) == -EACCES,
	      "write to the read-only system root is refused");

	check(zd_fs_resolve(session, ZD_PATH_SYSTEM_APPS "/hello.llext", false, out,
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
	struct zd_app_instance *owner_a = (struct zd_app_instance *)0xA;
	struct zd_app_instance *owner_b = (struct zd_app_instance *)0xB;
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

void zd_selftest_run(const struct zd_session *session)
{
	failures = 0;

	test_fs_scope(session);
	test_handles();

	if (failures == 0) {
		LOG_INF("selftest: all checks passed");
	} else {
		LOG_ERR("selftest: %u check(s) FAILED", failures);
	}
}
