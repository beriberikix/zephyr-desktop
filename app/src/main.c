/*
 * zephyr-desktop — boot and the desktop event loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "chrome/theme.h"
#include "host/session.h"
#include "host/storage.h"
#include "loader/app_loader.h"
#include "shell/desktop.h"
#include "shell/launcher.h"
#include "shell/taskbar.h"
#include "wm/wm.h"

LOG_MODULE_REGISTER(zd_main, CONFIG_ZD_LOG_LEVEL);

static struct zd_layers layers;
static struct zd_wm wm;
static struct zd_session session;

/*
 * Placeholder app binaries so discovery and the launcher have something real to
 * find before the loader exists. These are not valid ELF files and are never
 * loaded -- milestone F replaces them with a genuine hello.llext seeded from the
 * build. What is real here is the path: opendir, readdir, suffix match.
 */
static void seed_placeholder_apps(void)
{
	static const char *const names[] = { "hello", "notes" };
	char path[ZD_PATH_MAX];
	struct fs_file_t file;

	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		int ret = snprintf(path, sizeof(path), "%s/%s%s", ZD_PATH_SYSTEM_APPS,
				   names[i], ZD_APP_SUFFIX);

		if (ret < 0 || ret >= (int)sizeof(path)) {
			continue;
		}

		fs_file_t_init(&file);
		ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
		if (ret != 0) {
			LOG_WRN("could not seed %s (%d)", path, ret);
			continue;
		}
		fs_write(&file, "placeholder", 11);
		fs_close(&file);
	}
}

static void on_launcher_clicked(void *user_data)
{
	ARG_UNUSED(user_data);
	zd_launcher_toggle();
}

static void on_app_chosen(const struct zd_app_entry *entry, void *user_data)
{
	struct zd_wm *wm_ = user_data;

	/* Milestone F replaces this with load -> bringup -> manifest -> init.
	 * Opening a window named after the entry at least proves the path from
	 * a file on disk to a window on screen is continuous.
	 */
	zd_wm_window_create(wm_, entry->name, NULL);
}

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	int ret;

	if (!device_is_ready(display)) {
		LOG_ERR("display device %s not ready", display->name);
		return -ENODEV;
	}

	ret = zd_storage_init();
	if (ret != 0) {
		LOG_ERR("storage init failed (%d); the desktop has no apps", ret);
	}

	ret = zd_session_init(&session, 1000, "user");
	if (ret != 0) {
		LOG_ERR("session init failed (%d)", ret);
		return ret;
	}
	zd_storage_ensure_home(session.home);
	seed_placeholder_apps();

	lvgl_lock();
	zd_theme_init();
	zd_desktop_init(&layers);
	zd_taskbar_init(layers.panel, on_launcher_clicked, NULL);
	zd_wm_init(&wm, &layers);
	zd_wm_desktop_attach_events(&wm);
	zd_launcher_init(&layers, &session, on_app_chosen, &wm);
	lvgl_unlock();

	display_blanking_off(display);

	LOG_INF("zephyr-desktop up on %s", display->name);

	while (true) {
		/* Deferred destruction runs here, at the top of the loop and
		 * outside LVGL dispatch. Nothing is ever deleted from inside an
		 * event callback -- see CLAUDE.md.
		 */
		lvgl_lock();
		zd_wm_reap(&wm);
		uint32_t sleep_ms = lv_timer_handler();
		lvgl_unlock();

		k_msleep(MIN(sleep_ms, CONFIG_ZD_TICK_MAX_MS));
	}

	return 0;
}
