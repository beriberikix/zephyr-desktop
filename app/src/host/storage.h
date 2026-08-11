/*
 * zephyr-desktop — filesystem mount and layout.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_STORAGE_H_
#define ZD_HOST_STORAGE_H_

#include <zd/app_abi.h> /* ZD_PATH_MAX */

/*
 * Everything hangs off one root. FATFS insists a mount point be "/<VOLUME>:",
 * with the volume string generated from the devicetree disk-name, so paths are
 * not free-form: under QEMU the root is "/RAM:" and on an SD-backed board it
 * would be "/SD:". Apps never see this -- they go through zd_session_path() --
 * which is exactly why that indirection exists.
 */
#define ZD_FS_ROOT CONFIG_ZD_FS_ROOT

#define ZD_PATH_SYSTEM       ZD_FS_ROOT "/system"
#define ZD_PATH_SYSTEM_ZAPPS ZD_FS_ROOT "/system/zapps"
#define ZD_PATH_SYSTEM_SHARE ZD_FS_ROOT "/system/share"
#define ZD_PATH_HOME_BASE    ZD_FS_ROOT "/home"
#define ZD_PATH_TMP          ZD_FS_ROOT "/tmp"

/** File extension every app binary carries. */
#define ZD_ZAPP_SUFFIX ".llext"

/**
 * @brief Mount the app filesystem and ensure the directory layout exists.
 *
 * Formats on first boot when the volume has no filesystem. Idempotent.
 */
int zd_storage_init(void);

/** Create a session's home directory and its zapps/ subdirectory. */
int zd_storage_ensure_home(const char *home);

#endif /* ZD_HOST_STORAGE_H_ */
