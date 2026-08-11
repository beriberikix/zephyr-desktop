/*
 * zephyr-desktop — mount the zapp filesystem, create the layout.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/logging/log.h>
#include <ff.h>

#include "storage.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

static FATFS fat_fs;

static struct fs_mount_t mount_point = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = ZD_FS_ROOT,
#ifndef CONFIG_ZD_FS_AUTOFORMAT
	/* fat_fs.c formats on FR_NO_FILESYSTEM unless this flag is set, and it
	 * is the ONLY check it makes. Kconfig is no defence here:
	 * FS_FATFS_MOUNT_MKFS defaults to y and selects FS_FATFS_MKFS, so a
	 * board fragment setting them to n is silently overridden.
	 */
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
#endif
};

static int ensure_dir(const char *path)
{
	struct fs_dirent entry;
	int ret;

	ret = fs_stat(path, &entry);
	if (ret == 0) {
		return entry.type == FS_DIR_ENTRY_DIR ? 0 : -ENOTDIR;
	}

	ret = fs_mkdir(path);
	if (ret != 0 && ret != -EEXIST) {
		LOG_ERR("mkdir %s failed (%d)", path, ret);
		return ret;
	}

	return 0;
}

int zd_storage_init(void)
{
	static const char *const dirs[] = {
		ZD_PATH_SYSTEM,       ZD_PATH_SYSTEM_ZAPPS, ZD_PATH_SYSTEM_SHARE,
		ZD_PATH_HOME_BASE,    ZD_PATH_TMP,
	};
	int ret;

	ret = fs_mount(&mount_point);
	if (ret != 0) {
		LOG_ERR("mount %s failed (%d)", ZD_FS_ROOT, ret);
		if (!IS_ENABLED(CONFIG_ZD_FS_AUTOFORMAT)) {
			LOG_ERR("auto-format is off; the volume must already hold a "
				"FAT filesystem");
		}
		return ret;
	}

	LOG_INF("mounted %s", ZD_FS_ROOT);

	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		ret = ensure_dir(dirs[i]);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

int zd_storage_ensure_home(const char *home)
{
	char apps[ZD_PATH_MAX];
	int ret;

	ret = ensure_dir(home);
	if (ret != 0) {
		return ret;
	}

	ret = snprintf(apps, sizeof(apps), "%s/zapps", home);
	if (ret < 0 || ret >= (int)sizeof(apps)) {
		return -ENAMETOOLONG;
	}

	return ensure_dir(apps);
}
