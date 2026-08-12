/*
 * zephyr-desktop — what time the desktop thinks it is.
 *
 * One answer, shared. The taskbar has shown a clock since milestone B and the
 * arithmetic lived in taskbar.c, where nothing else could reach it; Notepad's
 * Time/Date needs the same number, and two independent fictions disagreeing on
 * screen would be worse than either alone.
 *
 * It IS a fiction, on the targets that have run so far. qemu_cortex_a53 has no
 * RTC node and Zephyr has no PL031 driver, so this counts up from a fixed start
 * time rather than showing 00:00 since boot -- which reads as a stopwatch, not
 * a desktop. A board with an RTC replaces the body of one function and every
 * caller becomes correct at once, which is the whole reason this is a service
 * rather than a local helper.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_CLOCK_H_
#define ZD_HOST_CLOCK_H_

#include <zd/zapp_abi.h>

/** Fill @p out with the desktop's current time. Never fails. */
void zd_clock_now(struct zd_time *out);

#endif /* ZD_HOST_CLOCK_H_ */
