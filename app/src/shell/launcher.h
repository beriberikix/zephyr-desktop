/*
 * zephyr-desktop — the Start menu.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_SHELL_LAUNCHER_H_
#define ZD_SHELL_LAUNCHER_H_

#include <lvgl.h>

#include "desktop.h"
#include "../host/session.h"
#include "../loader/zapp_loader.h"

/**
 * Invoked when a menu entry is chosen.
 *
 * Wired to the loader since milestone F -- letters index docs/history.md.
 */
typedef void (*zd_launch_cb_t)(const struct zd_zapp_entry *entry, void *user_data);

void zd_launcher_init(struct zd_layers *layers, const struct zd_session *session,
		      zd_launch_cb_t cb, void *cb_arg);

/** Rescan the zapp directories and rebuild the menu contents. */
void zd_launcher_refresh(void);

/** Show or hide the menu. Safe to call from an event callback. */
void zd_launcher_toggle(void);
void zd_launcher_hide(void);

#endif /* ZD_SHELL_LAUNCHER_H_ */
