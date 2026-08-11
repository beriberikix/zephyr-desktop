/*
 * zephyr-desktop — install built-in apps into the filesystem at boot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_LOADER_SEED_H_
#define ZD_LOADER_SEED_H_

/**
 * @brief Write the built-in app binaries to /system/zapps if absent.
 *
 * The .llext files are embedded in the image as C arrays at build time. This is
 * a stand-in for an installer, not a shortcut around the loader: what gets
 * written is a genuine build artifact, and everything downstream -- discovery,
 * llext_fs_loader, relocation -- reads it back off the filesystem exactly as it
 * would read a file that arrived on an SD card.
 *
 * Once apps are built out-of-tree against the llext EDK, this is replaced by
 * QEMU's fw_cfg channel and the desktop image stops needing a rebuild per app.
 */
#ifdef CONFIG_ZD_SEED_BUILTIN_APPS
int zd_seed_install(void);
#else
static inline int zd_seed_install(void)
{
	return 0;
}
#endif

#endif /* ZD_LOADER_SEED_H_ */
