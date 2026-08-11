/*
 * zephyr-desktop — session and user context.
 *
 * There is exactly one session in the MVP, but nothing here assumes that. No
 * global "current user" exists: every host-API call carries a zd_zapp_ctx_t, the
 * ctx points at its instance, and the instance points at its session. Adding a
 * second session is then a login screen, not a refactor.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_SESSION_H_
#define ZD_HOST_SESSION_H_

#include <stdbool.h>
#include <stdint.h>

#include <zd/zapp_abi.h>

#define ZD_USER_MAX     16
#define ZD_FS_ROOT_MAX  4

/** Filesystem capabilities. Advisory: enforced by the shim, not by hardware. */
#define ZD_CAP_FS_SYSTEM_RO BIT(0)
#define ZD_CAP_FS_HOME_RW   BIT(1)
#define ZD_CAP_FS_TMP_RW    BIT(2)

struct zd_fs_root {
	char path[ZD_PATH_MAX];
	bool writable;
};

struct zd_session {
	uint32_t uid;
	char user[ZD_USER_MAX];
	char home[ZD_PATH_MAX];
	uint32_t caps;

	/* The roots this session may touch, in longest-prefix order. */
	struct zd_fs_root roots[ZD_FS_ROOT_MAX];
	uint8_t root_count;
};

/** Build the single MVP session. Paths hang off CONFIG_ZD_FS_ROOT. */
int zd_session_init(struct zd_session *session, uint32_t uid, const char *user);

/**
 * @brief Resolve a well-known directory for this session.
 *
 * @return 0, or -ENOSPC if the path does not fit in @p out_len.
 */
int zd_session_path(const struct zd_session *session, enum zd_dir dir, char *out,
		    size_t out_len);

#endif /* ZD_HOST_SESSION_H_ */
