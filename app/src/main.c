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

#include "chrome/menu.h"
#include "chrome/theme.h"
#include "input/keys.h"
#include "host/session.h"
#include "host/storage.h"
#include "host/text_api.h"
#include "loader/zapp_instance.h"
#include "loader/zapp_loader.h"
#include "loader/seed.h"
#include "shell/desktop.h"
#include "shell/dialog.h"
#include "shell/launcher.h"
#include "shell/osk.h"
#include "shell/taskbar.h"
#include "shell/tasklist.h"
#include "selftest.h"
#include "smoke.h"
#include "wm/wm.h"

LOG_MODULE_REGISTER(zd_main, CONFIG_ZD_LOG_LEVEL);

static struct zd_layers layers;
static struct zd_wm wm;
static struct zd_session session;

static void on_launcher_clicked(void *user_data)
{
	ARG_UNUSED(user_data);
	zd_launcher_toggle();
}

static void on_app_chosen(const struct zd_zapp_entry *entry, void *user_data)
{
	ARG_UNUSED(user_data);

	/* A failed launch is an ordinary outcome, not a desktop failure: a bad
	 * ELF, an ABI mismatch or a full instance table all end up here. The
	 * loader has already logged why and unwound whatever it did.
	 */
	(void)zd_zapp_launch(entry);
}

/* Diagnostics for the input path, which is otherwise silent when it fails. */
static void report_pointer(void)
{
#if DT_HAS_CHOSEN(zephyr_touch)
	const struct device *touch = DEVICE_DT_GET(DT_CHOSEN(zephyr_touch));

	LOG_INF("touch device %s: %s", touch->name,
		device_is_ready(touch) ? "ready" : "NOT READY");
#else
	LOG_WRN("no zephyr,touch chosen -- nothing will be clickable");
#endif

	lv_indev_t *indev = lv_indev_get_next(NULL);
	unsigned int pointers = 0;

	while (indev != NULL) {
		if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
			pointers++;
		}
		indev = lv_indev_get_next(indev);
	}

	LOG_INF("LVGL pointer input devices: %u", pointers);
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
		LOG_ERR("storage init failed (%d); the desktop has no zapps", ret);
	}

	ret = zd_session_init(&session, 1000, "user");
	if (ret != 0) {
		LOG_ERR("session init failed (%d)", ret);
		return ret;
	}
	zd_storage_ensure_home(session.home);
	zd_seed_install();
	zd_selftest_run(&session);

	lvgl_lock();
	zd_theme_init();
	zd_desktop_init(&layers);
	zd_taskbar_init(layers.panel, on_launcher_clicked, NULL);
	zd_wm_init(&wm, &layers);
	zd_wm_desktop_attach_events(&wm);
	wm.on_client_destroyed = zd_zapp_on_client_destroyed;
	wm.on_client_focus = zd_zapp_on_client_focus;
	wm.on_client_click = zd_zapp_on_client_click;
	wm.on_client_resized = zd_zapp_on_client_resized;
	wm.on_client_minimized = zd_zapp_on_client_minimized;
	wm.on_client_key = zd_zapp_on_client_key;
	wm.on_client_text_key = zd_text_on_client_key;
	wm.on_client_close_request = zd_zapp_on_client_close_request;
	wm.on_client_close_stalled = zd_zapp_on_client_close_stalled;
	zd_keys_init(&wm);
	zd_tasklist_init(layers.panel, &wm);
	wm.on_client_list_changed = zd_tasklist_invalidate;
	zd_zapp_loader_init(&wm, &session);
	zd_launcher_init(&layers, &session, on_app_chosen, &wm);
	zd_osk_init(&layers);
	zd_menu_init(&layers);
	zd_dialog_init(&layers, &session);
	wm.on_key_grab = zd_dialog_key;
	zd_selftest_run_wm(&wm);
	zd_smoke_init(&wm, &session);
	lvgl_unlock();

	display_blanking_off(display);

	/* Say out loud whether there is anything to click with. A desktop whose
	 * pointer never initialised looks identical to one whose window manager
	 * is broken, and the difference is three lines of logging.
	 */
	report_pointer();

	LOG_INF("zephyr-desktop up on %s", display->name);

	while (true) {
		/* Deferred destruction runs here, at the top of the loop and
		 * outside LVGL dispatch. Nothing is ever deleted from inside an
		 * event callback -- see CLAUDE.md.
		 */
		lvgl_lock();
		/* Windows first, then instances: a zapp's LVGL objects must be
		 * gone before the code that created them is unmapped.
		 */
		zd_wm_reap(&wm);
		zd_zapp_reap();
		/* And the taskbar last, so it rebuilds from a stack the reap has
		 * already finished with rather than one still holding windows
		 * that are about to disappear.
		 */
		zd_tasklist_reap();
		/* And the dismissed menu, for the same reason as both of the
		 * above: choosing File -> Exit fires an event the zapp answers
		 * by closing its window, from a stack frame standing on the
		 * menu row that was clicked.
		 */
		zd_menu_reap();
		zd_dialog_reap();

		/* Keys arrive on Zephyr's input thread and everything they
		 * touch is LVGL's, so they are queued there and delivered
		 * here -- after the reaps, so a key never lands on a window
		 * that is already on its way out.
		 */
		zd_keys_pump();

		/* After the reaps, for the same reason the taskbar rebuild is:
		 * it launches and closes windows, which must never happen from
		 * inside LVGL dispatch. Compiles to nothing unless
		 * CONFIG_ZD_SMOKE_TEST is set.
		 */
		zd_smoke_tick();

		uint32_t sleep_ms = lv_timer_handler();
		lvgl_unlock();

		k_msleep(MIN(sleep_ms, CONFIG_ZD_TICK_MAX_MS));
	}

	return 0;
}
