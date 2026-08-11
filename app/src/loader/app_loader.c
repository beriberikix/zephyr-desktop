/*
 * zephyr-desktop — scanning app directories.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "app_loader.h"
#include "../host/storage.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

static bool has_app_suffix(const char *name)
{
	size_t name_len = strlen(name);
	size_t suffix_len = strlen(ZD_APP_SUFFIX);

	return name_len > suffix_len &&
	       strcmp(name + name_len - suffix_len, ZD_APP_SUFFIX) == 0;
}

/* "hello.llext" -> "hello". Truncates rather than failing: the stem is a label
 * for a menu, and the authoritative name comes from the manifest at load time.
 */
static void stem_of(const char *filename, char *out, size_t out_len)
{
	size_t len = strlen(filename) - strlen(ZD_APP_SUFFIX);

	if (len >= out_len) {
		len = out_len - 1;
	}
	memcpy(out, filename, len);
	out[len] = '\0';
}

static int scan_dir(const char *dir, bool system, struct zd_app_entry *out, size_t max,
		    size_t count)
{
	struct fs_dir_t dirp;
	struct fs_dirent entry;
	int ret;

	fs_dir_t_init(&dirp);

	ret = fs_opendir(&dirp, dir);
	if (ret != 0) {
		/* A missing app directory is normal, not an error: a session may
		 * simply have no user-installed apps.
		 */
		LOG_DBG("no app directory at %s (%d)", dir, ret);
		return (int)count;
	}

	while (count < max) {
		ret = fs_readdir(&dirp, &entry);
		if (ret != 0 || entry.name[0] == '\0') {
			break; /* error, or end of directory */
		}

		if (entry.type != FS_DIR_ENTRY_FILE || !has_app_suffix(entry.name)) {
			continue;
		}

		ret = snprintf(out[count].path, sizeof(out[count].path), "%s/%s", dir,
			       entry.name);
		if (ret < 0 || ret >= (int)sizeof(out[count].path)) {
			LOG_WRN("skipping %s/%s: path too long", dir, entry.name);
			continue;
		}

		stem_of(entry.name, out[count].name, sizeof(out[count].name));
		out[count].system = system;
		LOG_DBG("discovered app '%s' at %s", out[count].name, out[count].path);
		count++;
	}

	fs_closedir(&dirp);
	return (int)count;
}

int zd_apps_discover(const struct zd_session *session, struct zd_app_entry *out,
		     size_t max)
{
	char dir[ZD_PATH_MAX];
	int count = 0;
	int ret;

	if (out == NULL || max == 0) {
		return -EINVAL;
	}

	ret = zd_session_path(session, ZD_DIR_SYSTEM_APPS, dir, sizeof(dir));
	if (ret == 0) {
		count = scan_dir(dir, true, out, max, (size_t)count);
	}

	ret = zd_session_path(session, ZD_DIR_USER_APPS, dir, sizeof(dir));
	if (ret == 0) {
		count = scan_dir(dir, false, out, max, (size_t)count);
	}

	if ((size_t)count == max) {
		LOG_WRN("app list truncated at %zu entries; some apps are not shown", max);
	}

	LOG_INF("discovered %d app(s)", count);
	return count;
}
