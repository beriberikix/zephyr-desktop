/*
 * zephyr-desktop — boot and the desktop event loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "chrome/theme.h"
#include "shell/desktop.h"

LOG_MODULE_REGISTER(zd_main, CONFIG_ZD_LOG_LEVEL);

static struct zd_layers layers;

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display)) {
		LOG_ERR("display device %s not ready", display->name);
		return -ENODEV;
	}

	lvgl_lock();
	zd_theme_init();
	zd_desktop_init(&layers);
	lvgl_unlock();

	display_blanking_off(display);

	LOG_INF("zephyr-desktop up on %s", display->name);

	while (true) {
		/* Deferred destruction runs here, at the top of the loop and
		 * outside LVGL dispatch. Nothing is ever deleted from inside an
		 * event callback -- see CLAUDE.md.
		 */

		lvgl_lock();
		uint32_t sleep_ms = lv_timer_handler();
		lvgl_unlock();

		k_msleep(MIN(sleep_ms, CONFIG_ZD_TICK_MAX_MS));
	}

	return 0;
}
