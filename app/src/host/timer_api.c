/*
 * zephyr-desktop — implementation of zapp timers.
 *
 * Read timer_api.h first for why these are lv_timers. What follows is the one
 * decision that is not obvious from the header.
 *
 * AN LV_TIMER IS NEVER DELETED HERE, ONLY PAUSED. A zapp stopping a timer from
 * inside that timer's own callback is not an exotic case -- it is what "count
 * down and then stop" looks like, and Minesweeper does it twice, on the click
 * that wins and the click that loses. Deleting the lv_timer that
 * lv_timer_handler() is currently walking is the same class of bug as deleting
 * a window from inside its own event, and the project's answer to that
 * everywhere else is a deferred reap.
 *
 * There is no reap here because there does not need to be one. The pool is
 * fixed and small, so a stopped slot keeps its lv_timer paused and hands it
 * back to the next caller with a new period. Nothing is ever freed, so nothing
 * can be freed at the wrong moment, and the whole hazard stops existing rather
 * than being managed. The cost is CONFIG_ZD_MAX_TIMERS paused timers on LVGL's
 * list for the life of the boot, which lv_timer_handler() skips in a compare.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include "timer_api.h"
#include "../loader/zapp_instance.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

struct timer_rec {
	lv_timer_t *lv;
	struct zd_zapp_instance *owner; /**< NULL when the slot is free */
	uint16_t id;
};

static struct timer_rec timers[CONFIG_ZD_MAX_TIMERS];
static uint32_t live_timers;

static void timer_fired(lv_timer_t *lv)
{
	struct timer_rec *rec = lv_timer_get_user_data(lv);
	struct zd_event ev = {
		.type = ZD_EV_TIMER,
		/* No window. A timer belongs to the instance, and an instance
		 * may own four windows; guessing one would be worse than saying
		 * nothing. The ABI states this.
		 */
		.win = NULL,
	};

	if (rec == NULL || rec->owner == NULL) {
		return;
	}

	ev.timer.id = rec->id;
	zd_zapp_dispatch(rec->owner, &ev);
}

static struct timer_rec *find(struct zd_zapp_instance *owner, uint16_t id)
{
	for (size_t i = 0; i < ARRAY_SIZE(timers); i++) {
		if (timers[i].owner == owner && timers[i].id == id) {
			return &timers[i];
		}
	}

	return NULL;
}

int zd_timer_start(struct zd_zapp_instance *owner, uint32_t period_ms, uint16_t id)
{
	struct timer_rec *rec;

	if (owner == NULL) {
		return -EINVAL;
	}

	period_ms = MAX(period_ms, (uint32_t)CONFIG_ZD_TIMER_MIN_MS);

	rec = find(owner, id);
	if (rec != NULL) {
		/* Re-arm rather than making a second one. A zapp restarting its
		 * clock on every new game should not have to remember whether
		 * the last one was ever stopped.
		 */
		lv_timer_set_period(rec->lv, period_ms);
		lv_timer_reset(rec->lv);
		lv_timer_resume(rec->lv);
		return 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(timers); i++) {
		if (timers[i].owner != NULL) {
			continue;
		}

		rec = &timers[i];

		if (rec->lv == NULL) {
			/* First use of this slot, and the only place an
			 * lv_timer is ever made. It starts running, which is
			 * harmless: nothing between here and the resume below
			 * turns LVGL's handler.
			 */
			rec->lv = lv_timer_create(timer_fired, period_ms, rec);
			if (rec->lv == NULL) {
				return -ENOSPC;
			}
		}

		rec->owner = owner;
		rec->id = id;
		live_timers++;

		lv_timer_set_period(rec->lv, period_ms);
		lv_timer_reset(rec->lv);
		lv_timer_resume(rec->lv);
		return 0;
	}

	LOG_WRN("timer table full (%d)", CONFIG_ZD_MAX_TIMERS);
	return -ENOSPC;
}

int zd_timer_stop(struct zd_zapp_instance *owner, uint16_t id)
{
	struct timer_rec *rec;

	if (owner == NULL) {
		return -EINVAL;
	}

	rec = find(owner, id);
	if (rec == NULL) {
		/* Not an error. "Make sure the clock is off" is a reasonable
		 * thing to say twice, and the caller has no cheap way to know.
		 */
		return 0;
	}

	lv_timer_pause(rec->lv);
	rec->owner = NULL;
	live_timers--;
	return 0;
}

void zd_timer_owner_gone(struct zd_zapp_instance *inst)
{
	for (size_t i = 0; i < ARRAY_SIZE(timers); i++) {
		if (timers[i].owner == inst) {
			lv_timer_pause(timers[i].lv);
			timers[i].owner = NULL;
			live_timers--;
		}
	}
}

uint32_t zd_timer_live_count(void)
{
	return live_timers;
}
