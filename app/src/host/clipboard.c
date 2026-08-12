/*
 * zephyr-desktop — the system clipboard. See clipboard.h for why it is this
 * small.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "clipboard.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/*
 * Static, not heap. The clipboard outlives every zapp by design, so a heap
 * allocation here would be one the desktop never frees and could fail at the
 * worst moment -- when a user has just pressed Ctrl+C and has no reason to
 * imagine it could not work. A fixed buffer always has room for something.
 */
static char board[CONFIG_ZD_CLIPBOARD_MAX + 1];
static uint32_t board_len;

int zd_clipboard_set(const char *text, uint32_t len)
{
	if (text == NULL) {
		return -EINVAL;
	}

	if (len > CONFIG_ZD_CLIPBOARD_MAX) {
		LOG_WRN("clipboard truncated: %u bytes offered, %d kept", len,
			CONFIG_ZD_CLIPBOARD_MAX);
		len = CONFIG_ZD_CLIPBOARD_MAX;
	}

	memcpy(board, text, len);
	board[len] = '\0';
	board_len = len;

	return (int)len;
}

int zd_clipboard_get(uint32_t from, char *buf, uint32_t len)
{
	uint32_t take;

	if (buf == NULL || len == 0) {
		return -EINVAL;
	}

	if (from >= board_len) {
		buf[0] = '\0';
		return 0;
	}

	take = MIN(board_len - from, MIN(len - 1, (uint32_t)CONFIG_ZD_FS_IO_CHUNK));
	memcpy(buf, board + from, take);
	buf[take] = '\0';

	return (int)take;
}

uint32_t zd_clipboard_length(void)
{
	return board_len;
}
