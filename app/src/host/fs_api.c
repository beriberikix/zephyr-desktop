/*
 * zephyr-desktop — zapp storage.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "bus_arb.h"
#include "fs_api.h"
#include "fs_shim.h"
#include "../wm/handle.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/* A name that does not fit is worse than an error: a file browser would list
 * something it could not then open. Fail the build instead.
 */
BUILD_ASSERT(MAX_FILE_NAME + 1 <= ZD_NAME_MAX,
	     "ZD_NAME_MAX cannot hold a filename this filesystem can produce");

/*
 * The slot remembers its own handle. Without it zd_fs_close_all() could close
 * the file but not invalidate the handle naming it, leaving the caller to
 * follow up with zd_handle_free_all() -- which the instance teardown does, but
 * which made the operation quietly incomplete anywhere else, and observably so
 * in the boot selftest.
 */
struct open_file {
	struct fs_file_t zfp;
	struct zd_zapp_instance *owner;
	uintptr_t handle;
	bool live;
};

struct open_dir {
	struct fs_dir_t zdp;
	struct zd_zapp_instance *owner;
	uintptr_t handle;
	bool live;
};

static struct open_file files[CONFIG_ZD_MAX_OPEN_FILES];
static struct open_dir dirs[CONFIG_ZD_MAX_OPEN_DIRS];

/* --- helpers ---------------------------------------------------------------- */

static uint32_t owner_open_count(const struct zd_zapp_instance *owner)
{
	uint32_t count = 0;

	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		if (files[i].live && files[i].owner == owner) {
			count++;
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		if (dirs[i].live && dirs[i].owner == owner) {
			count++;
		}
	}

	return count;
}

static int check_quota(const struct zd_zapp_instance *owner)
{
	if (owner_open_count(owner) >= CONFIG_ZD_MAX_OPEN_PER_ZAPP) {
		LOG_WRN("instance %p hit its open-file quota (%d)", (void *)owner,
			CONFIG_ZD_MAX_OPEN_PER_ZAPP);
		return -EMFILE;
	}

	return 0;
}

static void to_dirent(const struct fs_dirent *in, struct zd_dirent *out)
{
	/* The BUILD_ASSERT above guarantees this fits. */
	strncpy(out->name, in->name, sizeof(out->name) - 1);
	out->name[sizeof(out->name) - 1] = '\0';
	out->size = (uint32_t)in->size;
	out->type = in->type == FS_DIR_ENTRY_DIR ? ZD_DIRENT_DIR : ZD_DIRENT_FILE;
}

/* Translate the zapp's flag namespace. Kept explicit rather than assuming the
 * two sets happen to share bit positions, which they nearly but not quite do.
 */
static fs_mode_t to_fs_mode(uint32_t flags)
{
	fs_mode_t mode = 0;

	if (flags & ZD_O_READ) {
		mode |= FS_O_READ;
	}
	if (flags & ZD_O_WRITE) {
		mode |= FS_O_WRITE;
	}
	if (flags & ZD_O_CREATE) {
		mode |= FS_O_CREATE;
	}
	if (flags & ZD_O_APPEND) {
		mode |= FS_O_APPEND;
	}
	if (flags & ZD_O_TRUNC) {
		mode |= FS_O_TRUNC;
	}

	return mode;
}

static struct open_file *file_of(struct zd_zapp_instance *owner, uintptr_t handle)
{
	return zd_handle_deref(handle, ZD_HANDLE_FILE, owner);
}

static struct open_dir *dir_of(struct zd_zapp_instance *owner, uintptr_t handle)
{
	return zd_handle_deref(handle, ZD_HANDLE_DIR, owner);
}

/* --- files ------------------------------------------------------------------ */

int zd_fs_open(const struct zd_session *session, struct zd_zapp_instance *owner,
	       const char *path, uint32_t flags, uintptr_t *out_handle)
{
	char clean[ZD_PATH_MAX];
	struct open_file *slot = NULL;
	uintptr_t handle;
	bool writing = (flags & (ZD_O_WRITE | ZD_O_CREATE | ZD_O_TRUNC)) != 0;
	int ret;

	if (session == NULL || out_handle == NULL) {
		return -EINVAL;
	}

	/* A read-only open with no read flag asks for nothing. */
	if ((flags & (ZD_O_READ | ZD_O_WRITE)) == 0) {
		return -EINVAL;
	}

	ret = check_quota(owner);
	if (ret != 0) {
		return ret;
	}

	ret = zd_fs_resolve(session, path, writing, clean, sizeof(clean));
	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		if (!files[i].live) {
			slot = &files[i];
			break;
		}
	}

	if (slot == NULL) {
		LOG_ERR("open file table exhausted (%d slots)", CONFIG_ZD_MAX_OPEN_FILES);
		return -ENFILE;
	}

	fs_file_t_init(&slot->zfp);

	zd_bus_storage_acquire();
	ret = fs_open(&slot->zfp, clean, to_fs_mode(flags));
	zd_bus_storage_release();

	if (ret != 0) {
		return ret;
	}

	/* Only now that the file is genuinely open does it get a handle -- and if
	 * the registry is full the file has to be closed again, or the slot leaks
	 * with nothing able to refer to it.
	 */
	handle = zd_handle_alloc(ZD_HANDLE_FILE, slot, owner);
	if (handle == 0) {
		zd_bus_storage_acquire();
		(void)fs_close(&slot->zfp);
		zd_bus_storage_release();
		return -ENOMEM;
	}

	slot->owner = owner;
	slot->handle = handle;
	slot->live = true;
	*out_handle = handle;

	return 0;
}

int zd_fs_read(struct zd_zapp_instance *owner, uintptr_t handle, void *buf, uint32_t len)
{
	struct open_file *file = file_of(owner, handle);
	ssize_t ret;

	if (file == NULL || buf == NULL) {
		return -EBADF;
	}

	/* Short reads are part of the contract. See the ABI header: this bounds
	 * how long the desktop thread -- and, where they share a pin, the display
	 * -- is stopped by one call.
	 */
	len = MIN(len, (uint32_t)CONFIG_ZD_FS_IO_CHUNK);

	zd_bus_storage_acquire();
	ret = fs_read(&file->zfp, buf, len);
	zd_bus_storage_release();

	return (int)ret;
}

int zd_fs_write(struct zd_zapp_instance *owner, uintptr_t handle, const void *buf,
		uint32_t len)
{
	struct open_file *file = file_of(owner, handle);
	ssize_t ret;

	if (file == NULL || buf == NULL) {
		return -EBADF;
	}

	len = MIN(len, (uint32_t)CONFIG_ZD_FS_IO_CHUNK);

	zd_bus_storage_acquire();
	ret = fs_write(&file->zfp, buf, len);
	zd_bus_storage_release();

	return (int)ret;
}

int zd_fs_seek(struct zd_zapp_instance *owner, uintptr_t handle, int32_t offset, int whence)
{
	struct open_file *file = file_of(owner, handle);
	int ret;

	if (file == NULL) {
		return -EBADF;
	}

	if (whence != ZD_SEEK_SET && whence != ZD_SEEK_CUR && whence != ZD_SEEK_END) {
		return -EINVAL;
	}

	zd_bus_storage_acquire();
	ret = fs_seek(&file->zfp, offset, whence);
	zd_bus_storage_release();

	return ret;
}

int zd_fs_tell(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct open_file *file = file_of(owner, handle);
	off_t ret;

	if (file == NULL) {
		return -EBADF;
	}

	zd_bus_storage_acquire();
	ret = fs_tell(&file->zfp);
	zd_bus_storage_release();

	return (int)ret;
}

int zd_fs_sync(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct open_file *file = file_of(owner, handle);
	int ret;

	if (file == NULL) {
		return -EBADF;
	}

	zd_bus_storage_acquire();
	ret = fs_sync(&file->zfp);
	zd_bus_storage_release();

	return ret;
}

void zd_fs_close(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct open_file *file = file_of(owner, handle);

	if (file == NULL) {
		return;
	}

	zd_bus_storage_acquire();
	(void)fs_close(&file->zfp);
	zd_bus_storage_release();

	file->live = false;
	file->owner = NULL;
	file->handle = 0;
	zd_handle_free(handle);
}

/* --- directories ------------------------------------------------------------ */

int zd_fs_opendir(const struct zd_session *session, struct zd_zapp_instance *owner,
		  const char *path, uintptr_t *out_handle)
{
	char clean[ZD_PATH_MAX];
	struct open_dir *slot = NULL;
	uintptr_t handle;
	int ret;

	if (session == NULL || out_handle == NULL) {
		return -EINVAL;
	}

	ret = check_quota(owner);
	if (ret != 0) {
		return ret;
	}

	ret = zd_fs_resolve(session, path, false, clean, sizeof(clean));
	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		if (!dirs[i].live) {
			slot = &dirs[i];
			break;
		}
	}

	if (slot == NULL) {
		LOG_ERR("open directory table exhausted (%d slots)", CONFIG_ZD_MAX_OPEN_DIRS);
		return -ENFILE;
	}

	fs_dir_t_init(&slot->zdp);

	zd_bus_storage_acquire();
	ret = fs_opendir(&slot->zdp, clean);
	zd_bus_storage_release();

	if (ret != 0) {
		return ret;
	}

	handle = zd_handle_alloc(ZD_HANDLE_DIR, slot, owner);
	if (handle == 0) {
		zd_bus_storage_acquire();
		(void)fs_closedir(&slot->zdp);
		zd_bus_storage_release();
		return -ENOMEM;
	}

	slot->owner = owner;
	slot->handle = handle;
	slot->live = true;
	*out_handle = handle;

	return 0;
}

int zd_fs_readdir(struct zd_zapp_instance *owner, uintptr_t handle, struct zd_dirent *out)
{
	struct open_dir *dir = dir_of(owner, handle);
	struct fs_dirent entry;
	int ret;

	if (dir == NULL || out == NULL) {
		return -EBADF;
	}

	zd_bus_storage_acquire();
	ret = fs_readdir(&dir->zdp, &entry);
	zd_bus_storage_release();

	if (ret != 0) {
		return ret;
	}

	/* Zephyr signals the end of a directory with an empty name rather than a
	 * distinct return code. Turn it into one, so a zapp's loop is a plain
	 * `while (readdir(...) == 0)`.
	 */
	if (entry.name[0] == '\0') {
		return -ENOENT;
	}

	to_dirent(&entry, out);
	return 0;
}

void zd_fs_closedir(struct zd_zapp_instance *owner, uintptr_t handle)
{
	struct open_dir *dir = dir_of(owner, handle);

	if (dir == NULL) {
		return;
	}

	zd_bus_storage_acquire();
	(void)fs_closedir(&dir->zdp);
	zd_bus_storage_release();

	dir->live = false;
	dir->owner = NULL;
	dir->handle = 0;
	zd_handle_free(handle);
}

/* --- whole-path operations --------------------------------------------------- */

int zd_fs_stat(const struct zd_session *session, const char *path, struct zd_dirent *out)
{
	char clean[ZD_PATH_MAX];
	struct fs_dirent entry;
	int ret;

	if (session == NULL || out == NULL) {
		return -EINVAL;
	}

	ret = zd_fs_resolve(session, path, false, clean, sizeof(clean));
	if (ret != 0) {
		return ret;
	}

	zd_bus_storage_acquire();
	ret = fs_stat(clean, &entry);
	zd_bus_storage_release();

	if (ret != 0) {
		return ret;
	}

	to_dirent(&entry, out);
	return 0;
}

int zd_fs_mkdir(const struct zd_session *session, const char *path)
{
	char clean[ZD_PATH_MAX];
	int ret;

	if (session == NULL) {
		return -EINVAL;
	}

	ret = zd_fs_resolve(session, path, true, clean, sizeof(clean));
	if (ret != 0) {
		return ret;
	}

	zd_bus_storage_acquire();
	ret = fs_mkdir(clean);
	zd_bus_storage_release();

	return ret;
}

int zd_fs_unlink(const struct zd_session *session, const char *path)
{
	char clean[ZD_PATH_MAX];
	int ret;

	if (session == NULL) {
		return -EINVAL;
	}

	ret = zd_fs_resolve(session, path, true, clean, sizeof(clean));
	if (ret != 0) {
		return ret;
	}

	zd_bus_storage_acquire();
	ret = fs_unlink(clean);
	zd_bus_storage_release();

	return ret;
}

int zd_fs_rename(const struct zd_session *session, const char *from, const char *to)
{
	char clean_from[ZD_PATH_MAX];
	char clean_to[ZD_PATH_MAX];
	int ret;

	if (session == NULL) {
		return -EINVAL;
	}

	/* Both ends are writes: one loses a name, the other gains one. */
	ret = zd_fs_resolve(session, from, true, clean_from, sizeof(clean_from));
	if (ret != 0) {
		return ret;
	}

	ret = zd_fs_resolve(session, to, true, clean_to, sizeof(clean_to));
	if (ret != 0) {
		return ret;
	}

	zd_bus_storage_acquire();
	ret = fs_rename(clean_from, clean_to);
	zd_bus_storage_release();

	return ret;
}

/* --- teardown and accounting -------------------------------------------------- */

void zd_fs_close_all(struct zd_zapp_instance *owner)
{
	unsigned int leaked = 0;

	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		if (!files[i].live || files[i].owner != owner) {
			continue;
		}

		zd_bus_storage_acquire();
		(void)fs_close(&files[i].zfp);
		zd_bus_storage_release();

		zd_handle_free(files[i].handle);
		files[i].live = false;
		files[i].owner = NULL;
		files[i].handle = 0;
		leaked++;
	}

	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		if (!dirs[i].live || dirs[i].owner != owner) {
			continue;
		}

		zd_bus_storage_acquire();
		(void)fs_closedir(&dirs[i].zdp);
		zd_bus_storage_release();

		zd_handle_free(dirs[i].handle);
		dirs[i].live = false;
		dirs[i].owner = NULL;
		dirs[i].handle = 0;
		leaked++;
	}

	/* Say something when a zapp left files open: it is not fatal -- the
	 * handles and the file objects are both gone now -- but it is a bug in
	 * the zapp, and otherwise an invisible one.
	 */
	if (leaked > 0) {
		LOG_WRN("closed %u file(s) instance %p left open", leaked, (void *)owner);
	}
}

uint32_t zd_fs_open_count(void)
{
	uint32_t count = 0;

	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		count += files[i].live ? 1 : 0;
	}
	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		count += dirs[i].live ? 1 : 0;
	}

	return count;
}
