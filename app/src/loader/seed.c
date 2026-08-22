/*
 * zephyr-desktop — seeding built-in zapps onto the filesystem.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "seed.h"
#include "../host/bus_arb.h"
#include "../host/storage.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

static const uint8_t hello_llext[] = {
#include <hello_llext.inc>
};

static const uint8_t notes_llext[] = {
#include <notes_llext.inc>
};

static const uint8_t badabi_llext[] = {
#include <badabi_llext.inc>
};

static const uint8_t notepad_llext[] = {
#include <notepad_llext.inc>
};

static const uint8_t files_llext[] = {
#include <files_llext.inc>
};

static const uint8_t mines_llext[] = {
#include <mines_llext.inc>
};

#ifdef CONFIG_ZD_CLIPPY
static const uint8_t clippy_llext[] = {
#include <clippy_llext.inc>
};
#endif

struct builtin {
	const char *name;
	const uint8_t *data;
	size_t size;
};

static const struct builtin builtins[] = {
	{ .name = "hello", .data = hello_llext, .size = sizeof(hello_llext) },
	{ .name = "notes", .data = notes_llext, .size = sizeof(notes_llext) },
	{ .name = "notepad", .data = notepad_llext, .size = sizeof(notepad_llext) },
	{ .name = "files", .data = files_llext, .size = sizeof(files_llext) },
	{ .name = "mines", .data = mines_llext, .size = sizeof(mines_llext) },
#ifdef CONFIG_ZD_CLIPPY
	/* Follows ZD_NET by default: seeding an assistant that cannot make a
	 * request would normally put a zapp in the launcher whose only possible
	 * answer is -ENOSYS. ZD_CLIPPY can be forced on without the TCP stack
	 * when that is exactly what is wanted -- see the Kconfig help.
	 */
	{ .name = "clippy", .data = clippy_llext, .size = sizeof(clippy_llext) },
#endif
	/* Installed on purpose: the ABI version gate is only proven by a zapp
	 * that has to be refused. See zapps/badabi.
	 */
	{ .name = "badabi", .data = badabi_llext, .size = sizeof(badabi_llext) },
};

/*
 * Is the installed copy byte-for-byte the built-in one?
 *
 * Compared properly rather than by size, which is what this used to do and
 * which is not the same question. A rebuilt zapp that happens to land on the
 * same length -- an ABI minor bump changes one constant in the manifest -- was
 * silently kept, so the desktop ran last build's extension against this build's
 * host and reported it in a log line nobody was reading. Fifteen kilobytes of
 * reads at boot is a cheap price for never wondering which binary is running.
 */
static bool same_content(const char *path, const struct builtin *app)
{
	struct fs_dirent entry;
	struct fs_file_t file;
	uint8_t buf[128];
	size_t off = 0;
	bool same = true;

	if (fs_stat(path, &entry) != 0 || entry.size != app->size) {
		return false;
	}

	fs_file_t_init(&file);
	if (fs_open(&file, path, FS_O_READ) != 0) {
		return false;
	}

	while (off < app->size) {
		size_t want = MIN(sizeof(buf), app->size - off);
		ssize_t got = fs_read(&file, buf, want);

		if (got <= 0 || memcmp(buf, app->data + off, (size_t)got) != 0) {
			same = false;
			break;
		}
		off += (size_t)got;
	}

	fs_close(&file);
	return same;
}

static int install_one(const struct builtin *app)
{
	char path[ZD_PATH_MAX];
	struct fs_dirent entry;
	struct fs_file_t file;
	bool existed;
	ssize_t written;
	int ret;

	ret = snprintf(path, sizeof(path), "%s/%s%s", ZD_PATH_SYSTEM_ZAPPS, app->name,
		       ZD_ZAPP_SUFFIX);
	if (ret < 0 || ret >= (int)sizeof(path)) {
		return -ENAMETOOLONG;
	}

	existed = fs_stat(path, &entry) == 0;

	if (existed && same_content(path, app)) {
		LOG_DBG("%s already installed", path);
		return 0;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret != 0) {
		LOG_ERR("cannot open %s (%d)", path, ret);
		return ret;
	}

	written = fs_write(&file, app->data, app->size);
	fs_close(&file);

	if (written < 0 || (size_t)written != app->size) {
		LOG_ERR("short write installing %s (%zd of %zu)", path, written, app->size);
		return -EIO;
	}

	/* Loud when it replaces something: overwriting a file already on the
	 * volume is the one thing here a user could be surprised by, and the
	 * surprise they get otherwise is a stale zapp.
	 */
	if (existed) {
		LOG_WRN("replaced %s with the built-in copy (%zu bytes)", path, app->size);
	} else {
		LOG_INF("installed %s (%zu bytes)", path, app->size);
	}

	return 0;
}

static int seed_install(void)
{
	int ret = 0;

	for (size_t i = 0; i < ARRAY_SIZE(builtins); i++) {
		int one = install_one(&builtins[i]);

		if (one != 0 && ret == 0) {
			ret = one;
		}
	}

	return ret;
}

int zd_seed_install(void)
{
	int ret;

	zd_bus_storage_acquire();
	ret = seed_install();
	zd_bus_storage_release();

	return ret;
}
