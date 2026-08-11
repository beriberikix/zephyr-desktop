/*
 * zephyr-desktop — boot-time negative checks.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_SELFTEST_H_
#define ZD_SELFTEST_H_

#include "host/session.h"

#ifdef CONFIG_ZD_SELFTEST
void zd_selftest_run(const struct zd_session *session);
#else
static inline void zd_selftest_run(const struct zd_session *session)
{
	ARG_UNUSED(session);
}
#endif

#endif /* ZD_SELFTEST_H_ */
