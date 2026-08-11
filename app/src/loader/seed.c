/*
 * zephyr-desktop — seeding built-in apps onto the filesystem.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "seed.h"
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

struct builtin {
	const char *name;
	const uint8_t *data;
	size_t size;
};

static const struct builtin builtins[] = {
	{ .name = "hello", .data = hello_llext, .size = sizeof(hello_llext) },
	{ .name = "notes", .data = notes_llext, .size = sizeof(notes_llext) },
	/* Installed on purpose: the ABI version gate is only proven by an app
	 * that has to be refused. See zapps/badabi.
	 */
	{ .name = "badabi", .data = badabi_llext, .size = sizeof(badabi_llext) },
};

static int install_one(const struct builtin *app)
{
	char path[ZD_PATH_MAX];
	struct fs_dirent entry;
	struct fs_file_t file;
	ssize_t written;
	int ret;

	ret = snprintf(path, sizeof(path), "%s/%s%s", ZD_PATH_SYSTEM_ZAPPS, app->name,
		       ZD_ZAPP_SUFFIX);
	if (ret < 0 || ret >= (int)sizeof(path)) {
		return -ENAMETOOLONG;
	}

	/* Already installed and the right size: leave it alone, so a file the
	 * user replaced by hand survives a reboot.
	 */
	if (fs_stat(path, &entry) == 0 && entry.size == app->size) {
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

	LOG_INF("installed %s (%zu bytes)", path, app->size);
	return 0;
}

int zd_seed_install(void)
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
