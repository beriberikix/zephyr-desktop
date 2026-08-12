/*
 * zephyr-desktop — the headless launch/close smoke test.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_SMOKE_H_
#define ZD_SMOKE_H_

#include "host/session.h"
#include "wm/wm.h"

#ifdef CONFIG_ZD_SMOKE_TEST

/** Arm the smoke test. Call once, after the WM and loader are wired. */
void zd_smoke_init(struct zd_wm *wm, const struct zd_session *session);

/**
 * @brief Advance one step.
 *
 * Called from the desktop loop AFTER the reaps, for the same reason the taskbar
 * rebuild is: it launches and closes windows, and doing that from inside LVGL
 * dispatch is the one thing this project must not do.
 */
void zd_smoke_tick(void);

#else

#define zd_smoke_init(wm, session) ((void)0)
#define zd_smoke_tick()            ((void)0)

#endif /* CONFIG_ZD_SMOKE_TEST */

#endif /* ZD_SMOKE_H_ */
