/*
 * zephyr-desktop — modifier state and the hand-off to the window manager.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zd/zapp_abi.h>

#include "keys.h"
#include "../wm/wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

static struct zd_wm *keys_wm;
static uint16_t held_mods;

void zd_keys_init(struct zd_wm *wm)
{
	keys_wm = wm;
	held_mods = 0;
}

void zd_keys_modifier(uint16_t mod, bool held)
{
	if (held) {
		held_mods |= mod;
	} else {
		held_mods &= (uint16_t)~mod;
	}
}

uint16_t zd_keys_mods(void)
{
	return held_mods;
}

void zd_keys_clear_mods(void)
{
	held_mods = 0;
}

void zd_keys_press(uint32_t code, uint32_t unicode)
{
	if (keys_wm == NULL) {
		return;
	}

	LOG_DBG("key %u u+%04x mods %02x", code, unicode, held_mods);

	zd_wm_key(keys_wm, code, unicode, held_mods);
}
