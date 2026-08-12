/*
 * zephyr-desktop — the system clipboard.
 *
 * The first genuine desktop *service*: state the desktop holds on behalf of
 * whoever asks, outliving the zapp that put it there. Everything before this
 * was either per-window or per-instance, so cut and paste between two windows
 * is the first thing in the project that is a property of the desktop rather
 * than of anything running on it.
 *
 * One buffer, one format -- bytes, nominally text. No ownership negotiation, no
 * multiple flavours, no lazy rendering: X11's selection protocol is what
 * happens when those are taken seriously, and none of them earn their keep on a
 * single-user embedded shell.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_CLIPBOARD_H_
#define ZD_HOST_CLIPBOARD_H_

#include <stdint.h>

/**
 * @brief Replace the clipboard's contents.
 *
 * Truncates at CONFIG_ZD_CLIPBOARD_MAX rather than refusing, and says so in the
 * log. A copy that silently produced nothing would be worse.
 *
 * @return bytes stored, or a negative errno.
 */
int zd_clipboard_set(const char *text, uint32_t len);

/**
 * @brief Copy out from byte offset @p from.
 *
 * Short by contract, like every other bulk read in this ABI.
 *
 * @return bytes written excluding the terminator, 0 at the end, or -errno.
 */
int zd_clipboard_get(uint32_t from, char *buf, uint32_t len);

/** @return the number of bytes held. */
uint32_t zd_clipboard_length(void);

#endif /* ZD_HOST_CLIPBOARD_H_ */
