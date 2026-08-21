/*
 * clippy — shared between the zapp and its face.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_ZAPP_CLIPPY_H_
#define ZD_ZAPP_CLIPPY_H_

#include <stdint.h>

enum clippy_mood {
	CLIPPY_IDLE = 0,
	CLIPPY_BUSY,
	CLIPPY_DONE,
	CLIPPY_SAD,
};

/**
 * @brief The paperclip for this mood at this tick.
 *
 * @param tick free-running animation counter; the frame set wraps it, so a
 *             caller never has to know how many frames a mood has.
 * @return a static string. Not owned by the caller and valid forever.
 */
const char *clippy_face(enum clippy_mood mood, uint32_t tick);

#endif /* ZD_ZAPP_CLIPPY_H_ */
