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
