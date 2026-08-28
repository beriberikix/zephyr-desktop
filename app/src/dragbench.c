/*
 * zephyr-desktop — synthetic window drag, for measuring redraw cost.
 *
 * "It feels slow when I drag a window" is not a number, and every candidate
 * fix -- a cheaper pixel format, an accelerator, drawing an outline instead of
 * the window -- needs a before and an after. This produces one without a
 * finger on the panel, which is the same reason tools/qemu-drive.py exists.
 *
 * It times lv_refr_now() rather than lv_timer_handler(). The loop's handler
 * only renders when LVGL's refresh timer is due, so timing it in a tight loop
 * would mostly measure the timer not firing; lv_refr_now() renders the pending
 * invalidation immediately and nothing else. The cost of a drag frame is
 * therefore what this reports, with the 30 Hz cap and the loop's sleep both
 * taken out of the picture.
 *
 * Debug aid, off by default. See CONFIG_ZD_DRAG_BENCH. Do not leave it on: it
 * runs before the desktop is usable and spends a second of boot doing it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

/* LVGL's own account of what it is about to repaint. There is no public
 * accessor for it, and the alternative is inferring the invalidated area from
 * the frame time -- which is the thing being explained, so it would be circular.
 */
#include "display/lv_display_private.h"

#include "dragbench.h"
#include "wm/client.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/* Far enough to average over a drag rather than a moment, short enough that it
 * does not visibly delay boot.
 */
#define STEPS 48
#define STEP_PX 4

/* A window about the size the zapps open at, so the invalidated area is the
 * one a real drag produces rather than a best or worst case.
 */
#define WIN_W 360
#define WIN_H 260

struct stat {
	uint32_t min_us;
	uint32_t max_us;
	uint64_t total_us;
	uint64_t total_px; /* what LVGL said it would repaint */
	uint32_t areas;
	uint32_t n;
};

/* Sum the pending invalid areas, skipping the ones LVGL has folded into a
 * neighbour -- those are already counted in whatever swallowed them.
 */
static uint64_t pending_px(uint32_t *areas)
{
	lv_display_t *disp = lv_display_get_default();
	uint64_t px = 0;

	*areas = 0;
	for (uint32_t i = 0; i < disp->inv_p; i++) {
		if (disp->inv_area_joined[i]) {
			continue;
		}
		px += (uint64_t)lv_area_get_width(&disp->inv_areas[i]) *
		      lv_area_get_height(&disp->inv_areas[i]);
		(*areas)++;
	}

	return px;
}

static void record(struct stat *s, uint32_t us, uint64_t px, uint32_t areas)
{
	s->total_px += px;
	s->areas += areas;

	if (s->n == 0U || us < s->min_us) {
		s->min_us = us;
	}
	if (us > s->max_us) {
		s->max_us = us;
	}
	s->total_us += us;
	s->n++;
}

static void report(const char *what, const struct stat *s)
{
	uint32_t mean;

	if (s->n == 0U) {
		return;
	}

	mean = (uint32_t)(s->total_us / s->n);

	/* FPS from the mean, integer-only: a frame costing `mean` us runs at
	 * 1000000/mean per second if nothing else competes.
	 */
	LOG_INF("dragbench: %s  min %u.%03u ms  mean %u.%03u ms  max %u.%03u ms  (%u fps)  "
		"%u px/frame in %u area(s)",
		what, s->min_us / 1000U, s->min_us % 1000U, mean / 1000U, mean % 1000U,
		s->max_us / 1000U, s->max_us % 1000U, mean ? 1000000U / mean : 0U,
		(uint32_t)(s->total_px / s->n), s->areas / s->n);
}

/* Render whatever is pending and return what it cost. */
static uint32_t refresh_us(void)
{
	uint32_t t0 = k_cycle_get_32();

	lv_refr_now(NULL);

	return k_cyc_to_us_near32(k_cycle_get_32() - t0);
}

void zd_dragbench_run(struct zd_wm *wm)
{
	lv_area_t geom = { .x1 = 40, .y1 = 120, .x2 = 40 + WIN_W - 1, .y2 = 120 + WIN_H - 1 };
	struct zd_client *client;
	struct stat drag = { 0 };
	struct stat full = { 0 };
	uint32_t x = geom.x1;
	uint32_t y = geom.y1;

	client = zd_wm_window_create(wm, "dragbench", &geom);
	if (client == NULL) {
		LOG_ERR("dragbench: no window");
		return;
	}

	/* Settle: draw the new window and anything else outstanding, so the
	 * first timed frame is a drag frame and not the window appearing.
	 */
	(void)refresh_us();
	(void)refresh_us();

	for (int i = 0; i < STEPS; i++) {
		/* Bounce rather than run off the edge, so every frame is a real
		 * move and none of them is clamped to a no-op.
		 */
		lv_area_t at;

		x += (uint32_t)STEP_PX;
		y += (uint32_t)((i / 12) % 2 == 0 ? STEP_PX : -STEP_PX);

		at.x1 = (int32_t)x;
		at.y1 = (int32_t)y;
		at.x2 = (int32_t)x + WIN_W - 1;
		at.y2 = (int32_t)y + WIN_H - 1;

		client->geom = at;
		zd_client_apply_pos(client);

		{
			uint32_t areas;
			uint64_t px = pending_px(&areas);

			record(&drag, refresh_us(), px, areas);
		}
	}


	/* For scale: what a whole-screen repaint costs. A drag frame should be
	 * well under this; if it is not, the invalidation is not being bounded
	 * to the window and that is the bug rather than the pixel rate.
	 */
	for (int i = 0; i < 4; i++) {
		lv_obj_invalidate(lv_screen_active());
		{
			uint32_t areas;
			uint64_t px = pending_px(&areas);

			record(&full, refresh_us(), px, areas);
		}
	}

	report("drag frame", &drag);
	report("full screen", &full);

	/* A frame that draws essentially nothing.
	 *
	 * Whatever this costs is the floor: it is what every frame pays before
	 * a single pixel of content is considered. If it is close to the drag
	 * frame, then no amount of drawing less will help and the cost is in
	 * presenting, not rendering.
	 */
	{
		lv_obj_t *dot = lv_obj_create(lv_screen_active());
		struct stat floor_ = { 0 };

		lv_obj_remove_style_all(dot);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		lv_obj_set_size(dot, 4, 4);
		lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
		(void)refresh_us();

		for (int i = 0; i < 16; i++) {
			uint32_t areas;
			uint64_t px;

			lv_obj_set_pos(dot, 600 + (i % 2), 400);
			px = pending_px(&areas);
			record(&floor_, refresh_us(), px, areas);
		}

		report("4x4 dot", &floor_);
		lv_obj_delete(dot);
	}

	/* The suspected fixed cost, timed on its own.
	 *
	 * A drag frame repaints 2,464 pixels and a full repaint 1,024,000 --
	 * 415x more -- for nothing like 415x the time, so most of a frame is
	 * something that does not scale with the area. The present path writes
	 * the whole framebuffer back out of the cache before handing it to the
	 * DMA, and that is the same cost whatever was drawn into it.
	 */
	{
		lv_display_t *disp = lv_display_get_default();
		lv_draw_buf_t *db = disp->buf_act;

		if (db != NULL && db->data != NULL) {
			uint32_t t0 = k_cycle_get_32();
			uint32_t us;

			sys_cache_data_flush_range(db->data, db->data_size);
			us = k_cyc_to_us_near32(k_cycle_get_32() - t0);

			LOG_INF("dragbench: cache writeback of %u KB costs %u.%03u ms",
				(uint32_t)(db->data_size / 1024U), us / 1000U, us % 1000U);
		}

		/*
		 * The present on its own -- and READ THIS NUMBER CAREFULLY,
		 * because it is not the present a real frame pays for.
		 *
		 * It hands the driver the same buffer every time, and the
		 * driver skips its wait when the buffer has not changed
		 * (prev_buf == buf in display_esp32_dsi.c). So this reports
		 * about half a millisecond and looks like proof that
		 * presenting is free. It is not: a real frame alternates
		 * buffers and waits two scanouts, which is the 29 ms the
		 * "4x4 dot" row measures. That row is the honest floor; this
		 * one is here because it was measured first and briefly
		 * argued the floor was somewhere else.
		 */
		if (db != NULL && db->data != NULL) {
			const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
			struct display_capabilities cap;
			struct display_buffer_descriptor desc;
			struct stat present = { 0 };

			display_get_capabilities(dev, &cap);
			desc.buf_size = db->data_size;
			desc.width = cap.x_resolution;
			desc.height = cap.y_resolution;
			desc.pitch = cap.x_resolution;
			desc.frame_incomplete = false;

			for (int i = 0; i < 8; i++) {
				uint32_t t0 = k_cycle_get_32();

				(void)display_write(dev, 0, 0, &desc, db->data);
				record(&present, k_cyc_to_us_near32(k_cycle_get_32() - t0), 0, 0);
			}

			report("present only", &present);
		}
	}

	/* Leave the desktop as it was found. The close is deferred like every
	 * other teardown here; the loop's reap runs immediately after this.
	 */
	zd_wm_window_close(client);
}
