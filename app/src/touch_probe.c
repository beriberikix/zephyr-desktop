/*
 * zephyr-desktop — direct touch-controller probe.
 *
 * The FT5336/FT6336 driver returns silently when an I2C read fails
 * (`if (r < 0) return r;`, no log), so a panel that answers on the bus but
 * never produces input events is indistinguishable from one whose every read
 * is erroring. This polls the touch-count register from its own thread and
 * reports both outcomes explicitly.
 *
 * Debug aid, off by default. See CONFIG_ZD_TOUCH_PROBE.
 *
 * Do not leave it on. It adds a second reader on the same I2C device at a
 * different cadence, so it changes the timing of the thing being measured --
 * and its own logging competes for the console. It is for answering one
 * question and then being switched off again.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

#define TOUCH_NODE DT_CHOSEN(zephyr_touch)

/* FT5336/FT6336 register map, as used by drivers/input/input_ft5336.c */
#define REG_TD_STATUS   0x02U
#define REG_P1_XH       0x03U
#define TOUCH_POINTS_MSK 0x0FU

#define PROBE_PERIOD_MS 50
#define REPORT_EVERY_MS 2000

static void touch_probe(void *a, void *b, void *c)
{
	const struct device *bus = DEVICE_DT_GET(DT_BUS(TOUCH_NODE));
	const uint16_t addr = DT_REG_ADDR(TOUCH_NODE);
	uint32_t polls = 0, errors = 0, touches = 0;
	int64_t next_report = k_uptime_get() + REPORT_EVERY_MS;
	int last_err = 0;

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	if (!device_is_ready(bus)) {
		LOG_ERR("touch probe: I2C bus %s not ready", bus->name);
		return;
	}

	LOG_INF("touch probe: polling 0x%02x on %s every %d ms", addr, bus->name,
		PROBE_PERIOD_MS);

	while (true) {
		uint8_t status = 0;
		int r;

		/* Log around the first few reads: if the bus wedges, the "before"
		 * line is the last thing we will ever see from this thread.
		 */
		if (polls < 3) {
			LOG_INF("touch probe: read #%u ...", polls);
		}

		r = i2c_reg_read_byte(bus, addr, REG_TD_STATUS, &status);

		if (polls < 3) {
			LOG_INF("touch probe: read #%u -> r=%d status=0x%02x", polls, r,
				status);
		}

		polls++;
		if (r < 0) {
			errors++;
			last_err = r;
		} else if ((status & TOUCH_POINTS_MSK) != 0) {
			uint8_t coords[4] = {0};

			touches++;
			if (i2c_burst_read(bus, addr, REG_P1_XH, coords, sizeof(coords)) == 0) {
				LOG_INF("touch probe: TOUCH points=%u raw=%u,%u",
					status & TOUCH_POINTS_MSK,
					((coords[0] & 0x0f) << 8) | coords[1],
					((coords[2] & 0x0f) << 8) | coords[3]);
			} else {
				LOG_INF("touch probe: TOUCH points=%u (coord read failed)",
					status & TOUCH_POINTS_MSK);
			}
		}

		if (k_uptime_get() >= next_report) {
			LOG_INF("touch probe: %u polls, %u errors (last %d), %u with touch",
				polls, errors, last_err, touches);
			polls = errors = touches = 0;
			next_report = k_uptime_get() + REPORT_EVERY_MS;
		}

		k_msleep(PROBE_PERIOD_MS);
	}
}

K_THREAD_DEFINE(zd_touch_probe, 2048, touch_probe, NULL, NULL, NULL, 7, 0, 1000);
