/*
 * zephyr-desktop — the zapp-facing filesystem, and the only place zapp storage
 * touches Zephyr's fs_*.
 *
 * fs_shim.c decides whether a path is allowed. This file is what finally asks
 * it: every entry point normalises and scopes the path through zd_fs_resolve(),
 * brackets the real call with the display/storage bus arbiter, and hands back a
 * generation-counted handle rather than a struct fs_file_t.
 *
 * Two shapes here are deliberate and easy to "tidy" into something worse.
 *
 * The owner is `struct zd_zapp_instance *` and is NEVER dereferenced -- it is a
 * quota and ownership tag only, with the session passed separately. That is why
 * zd_selftest_run() can drive this whole layer at boot, before any instance
 * exists, using the real session and a fake owner.
 *
 * Open objects come from fixed arrays, not the heap, for the same reason
 * struct zd_client does: a leaked file should announce itself as exhaustion
 * rather than hide as heap growth nobody measures.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_FS_API_H_
#define ZD_HOST_FS_API_H_

#include <stdint.h>

#include <zd/zapp_abi.h>

#include "session.h"

struct zd_zapp_instance;

/*
 * Handle-returning calls write a zd_handle_alloc() value to @p out_handle. All
 * of them return 0 or a negative errno unless noted.
 */

int zd_fs_open(const struct zd_session *session, struct zd_zapp_instance *owner,
	       const char *path, uint32_t flags, uintptr_t *out_handle);

/** @return bytes read (may be short), 0 at end of file, or a negative errno. */
int zd_fs_read(struct zd_zapp_instance *owner, uintptr_t handle, void *buf, uint32_t len);

/** @return bytes written (may be short), or a negative errno. */
int zd_fs_write(struct zd_zapp_instance *owner, uintptr_t handle, const void *buf,
		uint32_t len);

int zd_fs_seek(struct zd_zapp_instance *owner, uintptr_t handle, int32_t offset, int whence);

/** @return the current offset, or a negative errno. */
int zd_fs_tell(struct zd_zapp_instance *owner, uintptr_t handle);

int zd_fs_sync(struct zd_zapp_instance *owner, uintptr_t handle);

void zd_fs_close(struct zd_zapp_instance *owner, uintptr_t handle);

int zd_fs_opendir(const struct zd_session *session, struct zd_zapp_instance *owner,
		  const char *path, uintptr_t *out_handle);

/** @return 0 on an entry, -ENOENT at the end of the directory. */
int zd_fs_readdir(struct zd_zapp_instance *owner, uintptr_t handle, struct zd_dirent *out);

void zd_fs_closedir(struct zd_zapp_instance *owner, uintptr_t handle);

int zd_fs_stat(const struct zd_session *session, const char *path, struct zd_dirent *out);
int zd_fs_mkdir(const struct zd_session *session, const char *path);
int zd_fs_unlink(const struct zd_session *session, const char *path);
int zd_fs_rename(const struct zd_session *session, const char *from, const char *to);

/**
 * @brief Close every file and directory @p owner still holds.
 *
 * Called from instance teardown after fini() and before zd_handle_free_all(),
 * which only invalidates handles -- on its own it would leave FATFS holding the
 * underlying file objects open forever.
 */
void zd_fs_close_all(struct zd_zapp_instance *owner);

/** Open files plus open directories, desktop-wide. For leak assertions. */
uint32_t zd_fs_open_count(void);

#endif /* ZD_HOST_FS_API_H_ */
