/*
 * zephyr-desktop — session construction and path resolution.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "session.h"
#include "storage.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

static int add_root(struct zd_session *session, const char *path, bool writable)
{
	struct zd_fs_root *root;

	if (session->root_count >= ZD_FS_ROOT_MAX) {
		return -ENOSPC;
	}

	root = &session->roots[session->root_count];
	if (strlen(path) >= sizeof(root->path)) {
		return -ENAMETOOLONG;
	}

	strcpy(root->path, path);
	root->writable = writable;
	session->root_count++;
	return 0;
}

int zd_session_init(struct zd_session *session, uint32_t uid, const char *user)
{
	int ret;

	memset(session, 0, sizeof(*session));
	session->uid = uid;
	strncpy(session->user, user, ZD_USER_MAX - 1);

	ret = snprintf(session->home, sizeof(session->home), "%s/home/%s", ZD_FS_ROOT,
		       session->user);
	if (ret < 0 || ret >= (int)sizeof(session->home)) {
		return -ENAMETOOLONG;
	}

	session->caps = ZD_CAP_FS_SYSTEM_RO | ZD_CAP_FS_HOME_RW | ZD_CAP_FS_TMP_RW;

	ret = add_root(session, ZD_PATH_SYSTEM, false);
	if (ret == 0) {
		ret = add_root(session, session->home, true);
	}
	if (ret == 0) {
		ret = add_root(session, ZD_PATH_TMP, true);
	}
	if (ret != 0) {
		return ret;
	}

	LOG_INF("session uid=%u user='%s' home='%s'", session->uid, session->user,
		session->home);
	return 0;
}

int zd_session_path(const struct zd_session *session, enum zd_dir dir, char *out,
		    size_t out_len)
{
	int ret;

	switch (dir) {
	case ZD_DIR_HOME:
		ret = snprintf(out, out_len, "%s", session->home);
		break;
	case ZD_DIR_SYSTEM_APPS:
		ret = snprintf(out, out_len, "%s", ZD_PATH_SYSTEM_APPS);
		break;
	case ZD_DIR_USER_APPS:
		ret = snprintf(out, out_len, "%s/apps", session->home);
		break;
	case ZD_DIR_TMP:
		ret = snprintf(out, out_len, "%s", ZD_PATH_TMP);
		break;
	default:
		return -EINVAL;
	}

	return (ret < 0 || ret >= (int)out_len) ? -ENOSPC : 0;
}
