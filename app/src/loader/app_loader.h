/*
 * zephyr-desktop — app discovery.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_LOADER_APP_LOADER_H_
#define ZD_LOADER_APP_LOADER_H_

#include <stdbool.h>
#include <stddef.h>

#include <zd/app_abi.h>

#include "../host/session.h"

#define ZD_MAX_DISCOVERED 12

struct zd_app_entry {
	char path[ZD_PATH_MAX];        /**< absolute path to the .llext */
	char name[ZD_APP_NAME_MAX];    /**< filename stem, until the manifest is read */
	bool system;                   /**< found in /system/apps rather than a home */
};

/**
 * @brief Scan the session's app directories for *.llext.
 *
 * System apps are listed first. The display name is the filename stem; the real
 * name lives in the app's manifest and is only knowable after loading, which is
 * too expensive to do for every entry just to draw a menu.
 *
 * @return number of entries written, or a negative errno.
 */
int zd_apps_discover(const struct zd_session *session, struct zd_app_entry *out,
		     size_t max);

#endif /* ZD_LOADER_APP_LOADER_H_ */
