/*
 * zephyr-desktop — path normalisation and scope enforcement.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "fs_shim.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/*
 * Collapse "//" and "/./", and reject "..". Traversal is rejected rather than
 * resolved: resolving it would mean the shim and the filesystem could disagree
 * about what a path means, and the only safe answer to that is to not allow the
 * question.
 */
static int normalise(const char *path, char *out, size_t out_len)
{
	size_t w = 0;
	size_t r = 0;

	if (path == NULL || path[0] != '/') {
		return -EINVAL;
	}

	while (path[r] != '\0') {
		if (path[r] == '/') {
			/* Skip duplicate separators. */
			while (path[r] == '/') {
				r++;
			}

			/* Inspect the component that follows. */
			if (path[r] == '.') {
				if (path[r + 1] == '\0' || path[r + 1] == '/') {
					r++; /* "." -- drop it */
					continue;
				}
				if (path[r + 1] == '.' &&
				    (path[r + 2] == '\0' || path[r + 2] == '/')) {
					return -EINVAL; /* ".." -- refuse */
				}
			}

			if (path[r] == '\0') {
				break; /* trailing slash */
			}

			if (w + 1 >= out_len) {
				return -ENAMETOOLONG;
			}
			out[w++] = '/';
			continue;
		}

		if (w + 1 >= out_len) {
			return -ENAMETOOLONG;
		}
		out[w++] = path[r++];
	}

	if (w == 0) {
		if (out_len < 2) {
			return -ENAMETOOLONG;
		}
		out[w++] = '/';
	}

	out[w] = '\0';
	return 0;
}

/* True when @p path is @p root or lies beneath it. Compares whole components,
 * so "/RAM:/homework" does not match the root "/RAM:/home".
 */
static bool under_root(const char *path, const char *root)
{
	size_t len = strlen(root);

	if (strncmp(path, root, len) != 0) {
		return false;
	}

	return path[len] == '\0' || path[len] == '/';
}

int zd_fs_resolve(const struct zd_session *session, const char *path, bool for_write,
		  char *out, size_t out_len)
{
	char clean[ZD_PATH_MAX];
	int ret;

	ret = normalise(path, clean, sizeof(clean));
	if (ret != 0) {
		LOG_WRN("rejected path '%s' (%d)", path != NULL ? path : "(null)", ret);
		return ret;
	}

	for (uint8_t i = 0; i < session->root_count; i++) {
		const struct zd_fs_root *root = &session->roots[i];

		if (!under_root(clean, root->path)) {
			continue;
		}

		if (for_write && !root->writable) {
			LOG_WRN("uid=%u denied write to read-only root '%s'", session->uid,
				root->path);
			return -EACCES;
		}

		if (strlen(clean) >= out_len) {
			return -ENAMETOOLONG;
		}
		strcpy(out, clean);
		return 0;
	}

	LOG_WRN("uid=%u denied '%s': outside every permitted root", session->uid, clean);
	return -EACCES;
}
