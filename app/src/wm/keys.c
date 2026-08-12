/*
 * zephyr-desktop — where a key goes.
 *
 * The pointer's routing is LVGL's: it hit-tests, and focus.c turns the result
 * into policy. A key has no coordinates, so there is nothing to hit-test and
 * the policy is all there is. It lives here, in the WM, next to the focus state
 * it depends on, rather than being handed to LVGL's group/indev machinery and
 * then argued with.
 *
 * The whole policy:
 *
 *   1. Nothing focused -> nothing happens. There is no desktop-level keyboard
 *      shortcut yet, and inventing one here would be the wrong place for it.
 *   2. CTRL held -> straight to the zapp as ZD_EV_KEY. An accelerator has to
 *      work while the user is typing, so it must beat the text widget rather
 *      than queue behind it. This is the Win95 rule and it is why Ctrl+S is not
 *      an S.
 *   3. The focused window has an active text widget -> into it (see K4's
 *      wm->on_client_text_key hook), and the zapp is not told. A zapp does not
 *      want to be woken for every character any more than it wants to be woken
 *      for every pointer sample during a drag.
 *   4. Otherwise -> ZD_EV_KEY.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>

#include <zd/zapp_abi.h>

#include "wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

void zd_wm_key(struct zd_wm *wm, uint32_t code, uint32_t unicode, uint16_t mods)
{
	struct zd_client *client = wm->focused;

	/* A modal surface takes everything, including CTRL. An accelerator that
	 * still worked while a "save changes?" box was up would act on a window
	 * the user cannot currently see the state of.
	 */
	if (wm->on_key_grab != NULL && wm->on_key_grab(code, unicode, mods)) {
		return;
	}

	if (client == NULL || client->pending_destroy) {
		return;
	}

	if ((mods & ZD_MOD_CTRL) == 0 && wm->on_client_text_key != NULL &&
	    wm->on_client_text_key(client, code, unicode, mods)) {
		return; /* a text widget consumed it */
	}

	/* Then a list, if one has the keyboard. Second rather than first: the
	 * two cannot both be focused, so the order only decides what happens if
	 * a future widget kind forgets to clear the others -- and "the caret
	 * wins" is the right answer to that.
	 */
	if ((mods & ZD_MOD_CTRL) == 0 && wm->on_client_list_key != NULL &&
	    wm->on_client_list_key(client, code, unicode, mods)) {
		return;
	}

	if (wm->on_client_key != NULL) {
		wm->on_client_key(client, code, unicode, mods);
	}
}
