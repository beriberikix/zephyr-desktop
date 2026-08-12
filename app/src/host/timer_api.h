/*
 * zephyr-desktop — periodic wake-ups for zapps.
 *
 * The first thing in this desktop that lets a zapp run when the user has done
 * nothing. Everything up to ABI 0.6 was strictly reactive -- a zapp existed
 * inside a callback or it did not exist -- which was right for a text editor
 * and a browser and is not enough for anything with a clock in it.
 *
 * Built on lv_timer rather than k_timer or a thread, and that is the whole
 * safety argument. An lv_timer callback runs inside lv_timer_handler(), on the
 * desktop thread, holding the LVGL lock, in exactly the same place a click
 * lands. So a ZD_EV_TIMER is dispatched under the same in_zapp_callback guard,
 * obeys the same deferred-destruction rules, and needs no new reasoning at all.
 * A k_timer would fire on the system work queue and would be the keyboard bug
 * again: everything downstream is LVGL's, and only this thread may touch it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_TIMER_API_H_
#define ZD_HOST_TIMER_API_H_

#include <stdint.h>

struct zd_zapp_instance;

/**
 * @brief Start or re-arm one of @p owner's timers.
 *
 * @param period_ms raised to CONFIG_ZD_TIMER_MIN_MS if smaller. A zapp asking
 *                  to be woken every millisecond is asking the desktop to stop
 *                  drawing; the floor is the answer rather than a refusal,
 *                  because the caller wanted "often" and can have it.
 * @param id        the caller's, handed back in ev->timer.id. Starting one that
 *                  is already running re-arms it rather than making a second.
 *
 * @return 0, or -ENOSPC when every slot is taken.
 */
int zd_timer_start(struct zd_zapp_instance *owner, uint32_t period_ms, uint16_t id);

/** Stop it. Stopping one that is not running is not an error. */
int zd_timer_stop(struct zd_zapp_instance *owner, uint16_t id);

/**
 * @brief Stop every timer belonging to @p inst.
 *
 * Called from the instance teardown, beside zd_dialog_owner_gone() and for the
 * same reason: a timer left running would dispatch into an extension whose text
 * has been unmapped, which is the one failure mode this project treats as
 * unacceptable rather than untidy.
 */
void zd_timer_owner_gone(struct zd_zapp_instance *inst);

/** Running timers, for leak assertions. */
uint32_t zd_timer_live_count(void);

#endif /* ZD_HOST_TIMER_API_H_ */
