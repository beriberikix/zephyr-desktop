/*
 * zephyr-desktop — modifier state, the key queue, and the hand-off to the WM.
 *
 * THE QUEUE IS NOT AN OPTIMISATION. A key arrives on Zephyr's input thread, and
 * everything downstream of routing it -- the text widget, the dialog's filename
 * field, a zapp's event callback -- touches LVGL. Doing that from any thread but
 * the one running lv_timer_handler() races the whole UI, and doing it with the
 * desktop's own callbacks on the stack is worse.
 *
 * The first version called straight through and died within two keystrokes:
 * "ZEPHYR FATAL ERROR 2: Stack overflow on CPU 0, Current thread: input". The
 * stack was the symptom -- the input thread is small and immediate logging
 * formats on the caller's stack -- and the locking was the actual fault.
 *
 * So a key press is queued and drained from the desktop loop, which is exactly
 * how LVGL's own pointer indev works: the driver fills a message queue, and
 * lv_timer_handler() empties it. The pointer got this for free because Zephyr's
 * glue does it; keys are ours, so we do it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zd/zapp_abi.h>

#include "keys.h"
#include "../wm/wm.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

struct key_event {
	uint32_t code;
	uint32_t unicode;
	uint16_t mods;
};

/*
 * Bounded, and dropping is the right failure. A queue that blocked would stall
 * the input thread behind a UI that is not drawing; a queue that grew would
 * turn a stuck desktop into an out-of-memory one. Sixteen is several seconds of
 * fast typing at the desktop's 30 ms loop.
 */
K_MSGQ_DEFINE(key_queue, sizeof(struct key_event), CONFIG_ZD_KEY_QUEUE_LEN, 4);

static struct zd_wm *keys_wm;

/*
 * Modifier state belongs to the SOURCE side, not the drain side.
 *
 * It is written on whatever thread reported the key and read in the same call,
 * then snapshotted into the queued event. Reading it at drain time instead
 * would attribute a shift to whichever key happened to be draining when the
 * user let go.
 */
static uint16_t held_mods;

void zd_keys_init(struct zd_wm *wm)
{
	keys_wm = wm;
	held_mods = 0;
	k_msgq_purge(&key_queue);
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
	struct key_event ev = {
		.code = code,
		.unicode = unicode,
		.mods = held_mods,
	};

	if (k_msgq_put(&key_queue, &ev, K_NO_WAIT) != 0) {
		LOG_WRN("key dropped; the desktop loop is behind");
	}
}

void zd_keys_pump(void)
{
	struct key_event ev;

	if (keys_wm == NULL) {
		return;
	}

	while (k_msgq_get(&key_queue, &ev, K_NO_WAIT) == 0) {
		LOG_DBG("key %u u+%04x mods %02x", ev.code, ev.unicode, ev.mods);
		zd_wm_key(keys_wm, ev.code, ev.unicode, ev.mods);
	}
}
