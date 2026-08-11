/*
 * zephyr-desktop — the filesystem choke point.
 *
 * Every app filesystem call passes through here: normalise, reject traversal,
 * match against the session's permitted roots and their read/write mode, then
 * call Zephyr fs_*.
 *
 * Be clear about what this is. With no MMU isolation a loaded llext is trusted
 * code sharing the kernel address space, and Zephyr's own fs_open() is reachable
 * from an extension whenever CONFIG_LLEXT_EXPORT_SYMBOL_GROUP_SYSCALL is on. So
 * this is a *contract*, not a security boundary: it makes the shim the only
 * linkable route, not the only possible one. Real enforcement needs
 * CONFIG_USERSPACE and llext_add_domain(), which is why every entry point here
 * already carries a session -- those calls become syscalls without an app
 * changing a line.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_FS_SHIM_H_
#define ZD_HOST_FS_SHIM_H_

#include <stdbool.h>
#include <stddef.h>

#include "session.h"

/**
 * @brief Validate a path against a session's roots.
 *
 * @param session   the calling app's session
 * @param path      candidate absolute path
 * @param for_write true if the caller intends to modify
 * @param out       normalised path, written only on success
 * @param out_len   size of @p out
 *
 * @retval 0          allowed
 * @retval -EINVAL    malformed, relative, or contains a ".." component
 * @retval -EACCES    outside every root, or write to a read-only root
 * @retval -ENAMETOOLONG does not fit
 */
int zd_fs_resolve(const struct zd_session *session, const char *path, bool for_write,
		  char *out, size_t out_len);

#endif /* ZD_HOST_FS_SHIM_H_ */
