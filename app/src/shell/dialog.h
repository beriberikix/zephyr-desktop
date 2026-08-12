/*
 * zephyr-desktop — modal dialogs, provided by the desktop.
 *
 * "The text in the Untitled file has changed" and "Open which file?" are not
 * application problems. Every zapp that edits anything needs both, they must
 * look and behave the same everywhere, and the file picker needs to walk the
 * filesystem through the session's permission shim -- which is the desktop's
 * job, not the zapp's.
 *
 * Both are asynchronous. A zapp asks, gets control back immediately, and is
 * told the answer later with ZD_EV_DIALOG. There is no modal loop anywhere in
 * this project and there is not going to be: the desktop thread runs LVGL, and
 * blocking it to wait for a click would stop the thing being clicked from
 * drawing.
 *
 * Modality is enforced by a click-swallowing shade on the overlay layer, which
 * is the same mechanism a menu drop-down uses. That makes it SYSTEM modal
 * rather than application modal -- a simplification worth being explicit about,
 * since Win95 was the other way round.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_SHELL_DIALOG_H_
#define ZD_SHELL_DIALOG_H_

#include <stdbool.h>
#include <stdint.h>

#include <zd/zapp_abi.h>

#include "desktop.h"

struct zd_client;
struct zd_session;
struct zd_zapp_instance;

/** Build the shared dialog surface. Call once at boot. */
void zd_dialog_init(struct zd_layers *layers, const struct zd_session *session);

int zd_dialog_confirm(struct zd_zapp_instance *owner, struct zd_client *client,
		      const char *title, const char *msg, uint32_t buttons, uint16_t id);

int zd_dialog_file(struct zd_zapp_instance *owner, struct zd_client *client,
		   const char *title, enum zd_dir dir, uint32_t mode, uint16_t id);

/**
 * @brief The path the last file dialog produced.
 *
 * A separate call rather than a field in the event: ZD_PATH_MAX does not belong
 * in a union every event carries. The same "the event says what, a call says
 * how much" shape the rest of the ABI uses.
 */
int zd_dialog_get_path(struct zd_zapp_instance *owner, char *buf, uint32_t len);

/** Dismiss whatever is open, answering CANCEL. Safe from inside dispatch. */
void zd_dialog_cancel(void);

/** Delete the dismissed dialog's widgets. From the desktop loop only. */
void zd_dialog_reap(void);

/** Take the dialog down with the instance that asked for it. */
void zd_dialog_owner_gone(struct zd_zapp_instance *inst);

/**
 * @brief Offer a key to an open dialog.
 *
 * Installed as wm->on_key_grab. A dialog takes every key while it is up --
 * Enter and Escape as the default and cancel buttons, and everything else for
 * the filename field. That is what modal means for a keyboard.
 */
bool zd_dialog_key(uint32_t code, uint32_t unicode, uint16_t mods);

/** Is one up? */
bool zd_dialog_open(void);

#endif /* ZD_SHELL_DIALOG_H_ */
