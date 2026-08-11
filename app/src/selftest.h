/*
 * zephyr-desktop — boot-time negative checks.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_SELFTEST_H_
#define ZD_SELFTEST_H_

#include "host/session.h"

struct zd_wm;

#ifdef CONFIG_ZD_SELFTEST
void zd_selftest_run(const struct zd_session *session);

/**
 * @brief The window-manager checks, which need a WM.
 *
 * Separate from zd_selftest_run() only because of when it can run: the shim and
 * the handle registry are testable before the display exists, and are checked
 * that early on purpose. This one has to wait until zd_wm_init(), so it is
 * called from main() after the desktop is built and before the loop starts.
 */
void zd_selftest_run_wm(struct zd_wm *wm);
#else
static inline void zd_selftest_run(const struct zd_session *session)
{
	ARG_UNUSED(session);
}

static inline void zd_selftest_run_wm(struct zd_wm *wm)
{
	ARG_UNUSED(wm);
}
#endif

#endif /* ZD_SELFTEST_H_ */
