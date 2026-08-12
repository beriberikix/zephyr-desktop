/*
 * zephyr-desktop — the desktop clock. See clock.h for why it is a fiction.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "clock.h"

/*
 * Where the fiction starts. Any value does; a plausible mid-morning reads as a
 * clock, and 00:00 reads as a stopwatch.
 */
#define BASE_HOUR   9
#define BASE_MINUTE 41

void zd_clock_now(struct zd_time *out)
{
	int64_t secs = (int64_t)BASE_HOUR * 3600 + (int64_t)BASE_MINUTE * 60 +
		       k_uptime_get() / MSEC_PER_SEC;

	out->hour = (uint8_t)((secs / 3600) % 24);
	out->minute = (uint8_t)((secs / 60) % 60);
	out->second = (uint8_t)(secs % 60);
}
