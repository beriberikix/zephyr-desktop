/*
 * zephyr-desktop — arbitration for a storage bus that shares a pin with the
 * display.
 *
 * On most boards these two are independent and this is nothing. On the M5Stack
 * CoreS3 one pin is wired to both, and every filesystem access has to take the
 * pin back from the display first. Bracket a *whole logical operation* rather
 * than an individual fs_* call: acquire, do the work, release. Nesting is fine.
 *
 * The one rule: never call into LVGL while holding it. The display cannot draw
 * during the window, which is exactly the point.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_BUS_ARB_H_
#define ZD_HOST_BUS_ARB_H_

#ifdef CONFIG_ZD_SPI_DC_MISO_ARBITER

/** Point the shared pin at storage. Recursive; pair with a release. */
void zd_bus_storage_acquire(void);

/** Hand the shared pin back to the display. */
void zd_bus_storage_release(void);

#else

static inline void zd_bus_storage_acquire(void)
{
}

static inline void zd_bus_storage_release(void)
{
}

#endif /* CONFIG_ZD_SPI_DC_MISO_ARBITER */

#endif /* ZD_HOST_BUS_ARB_H_ */
