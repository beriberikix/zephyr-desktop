/*
 * zephyr-desktop — launch every zapp, close every zapp, prove nothing leaked.
 *
 * The boot selftest in selftest.c checks things that hold still: path scoping,
 * handle generations, chrome geometry. This checks the one thing it cannot,
 * because it takes several trips round the desktop loop to happen at all --
 * that a zapp can be loaded, given a window, closed, and unloaded, repeatedly,
 * and that every bounded resource comes back afterwards.
 *
 * It exists because the desktop is otherwise only testable with a pointer. A
 * headless QEMU run says the image booted; it says nothing about whether an
 * extension still loads, which is exactly what changes when the llext binary
 * type, the ABI minor or the seed's staleness check move. Milestone J shipped
 * two rounds of hand-testing against a stale binary for want of this.
 *
 * Off by default: it takes over the desktop for a few seconds at boot and
 * closes every window it finds. Turn it on for a build you are measuring, not
 * for one you are using.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "smoke.h"
#include "host/fs_api.h"
#include "loader/zapp_instance.h"
#include "loader/zapp_loader.h"
#include "wm/handle.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/* Ticks to wait for the loop to settle after a launch or a close. Generous:
 * a close is a request, a grace period, a reap and an unload, and the point of
 * the test is to observe the end state rather than to race it.
 */
#define SETTLE_TICKS 4
#define CLOSE_TICKS  ((CONFIG_ZD_CLOSE_GRACE_MS / CONFIG_ZD_TICK_MAX_MS) + 8)

enum smoke_phase {
	SMOKE_IDLE,
	SMOKE_LAUNCH,
	SMOKE_SETTLE,
	SMOKE_CLOSE,
	SMOKE_DRAIN,
	SMOKE_CHECK,
	SMOKE_DONE,
};

struct counters {
	uint32_t clients;
	uint32_t handles;
	uint32_t files;
	uint32_t instances;
};

static struct {
	struct zd_wm *wm;
	const struct zd_session *session;

	struct zd_zapp_entry entries[ZD_MAX_DISCOVERED];
	int count;

	struct counters baseline;
	enum smoke_phase phase;
	unsigned int round;
	unsigned int wait;
	unsigned int launched;
	unsigned int refused;
	unsigned int failures;
} smoke;

static void snapshot(struct counters *out)
{
	out->clients = zd_wm_client_count(smoke.wm);
	out->handles = zd_handle_live_count();
	out->files = zd_fs_open_count();
	out->instances = zd_zapp_instance_count();
}

static bool same_as_baseline(const struct counters *now)
{
	const struct counters *was = &smoke.baseline;

	return now->clients == was->clients && now->handles == was->handles &&
	       now->files == was->files && now->instances == was->instances;
}

void zd_smoke_init(struct zd_wm *wm, const struct zd_session *session)
{
	smoke.wm = wm;
	smoke.session = session;

	smoke.count = zd_zapps_discover(session, smoke.entries, ZD_MAX_DISCOVERED);
	if (smoke.count < 0) {
		LOG_ERR("SMOKE: discovery failed (%d)", smoke.count);
		smoke.count = 0;
	}

	snapshot(&smoke.baseline);
	smoke.phase = SMOKE_LAUNCH;

	LOG_INF("SMOKE: %d zapp(s), %d round(s); baseline %u client(s), %u handle(s), "
		"%u file(s)",
		smoke.count, CONFIG_ZD_SMOKE_ROUNDS, smoke.baseline.clients,
		smoke.baseline.handles, smoke.baseline.files);
}

/* Ask every zapp-owned window to go. Not zd_wm_window_close(): the polite form
 * is the one a user's click takes, so it is the one worth exercising -- and it
 * is the only way the grace-period path is ever covered headlessly.
 */
static unsigned int request_close_all(void)
{
	struct zd_client *client;
	struct zd_client *next;
	unsigned int asked = 0;

	SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&smoke.wm->stack, client, next, node) {
		if (client->owner == NULL || client->pending_destroy) {
			continue;
		}
		zd_wm_window_close_request(client);
		asked++;
	}

	return asked;
}

void zd_smoke_tick(void)
{
	struct counters now;

	if (smoke.wait > 0) {
		smoke.wait--;
		return;
	}

	switch (smoke.phase) {
	case SMOKE_LAUNCH:
		smoke.launched = 0;
		smoke.refused = 0;

		for (int i = 0; i < smoke.count; i++) {
			if (zd_zapp_launch(&smoke.entries[i]) == 0) {
				smoke.launched++;
			} else {
				/* badabi is installed precisely so that one
				 * entry here is always refused. A round where
				 * nothing was refused means the version gate
				 * stopped working.
				 */
				smoke.refused++;
			}
		}

		LOG_INF("SMOKE round %u: %u launched, %u refused", smoke.round + 1,
			smoke.launched, smoke.refused);

		smoke.phase = SMOKE_SETTLE;
		smoke.wait = SETTLE_TICKS;
		break;

	case SMOKE_SETTLE:
		snapshot(&now);
		LOG_INF("SMOKE round %u: up with %u window(s), %u handle(s)",
			smoke.round + 1, now.clients, now.handles);
		smoke.phase = SMOKE_CLOSE;
		break;

	case SMOKE_CLOSE:
		LOG_INF("SMOKE round %u: asked %u window(s) to close", smoke.round + 1,
			request_close_all());
		smoke.phase = SMOKE_DRAIN;
		smoke.wait = CLOSE_TICKS;
		break;

	case SMOKE_DRAIN:
		smoke.phase = SMOKE_CHECK;
		break;

	case SMOKE_CHECK:
		snapshot(&now);

		if (same_as_baseline(&now)) {
			LOG_INF("SMOKE round %u: clean", smoke.round + 1);
		} else {
			LOG_ERR("SMOKE round %u: LEAK -- %u/%u client(s), %u/%u handle(s), "
				"%u/%u file(s), %u/%u instance(s)",
				smoke.round + 1, now.clients, smoke.baseline.clients,
				now.handles, smoke.baseline.handles, now.files,
				smoke.baseline.files, now.instances,
				smoke.baseline.instances);
			smoke.failures++;
		}

		if (smoke.refused == 0) {
			LOG_ERR("SMOKE round %u: nothing was refused -- the ABI gate is "
				"not gating", smoke.round + 1);
			smoke.failures++;
		}

		smoke.round++;
		smoke.phase = smoke.round < CONFIG_ZD_SMOKE_ROUNDS ? SMOKE_LAUNCH
								  : SMOKE_DONE;
		break;

	case SMOKE_DONE:
		LOG_INF("SMOKE: %s after %u round(s)",
			smoke.failures == 0 ? "PASS" : "FAIL", smoke.round);
		smoke.phase = SMOKE_IDLE;
		break;

	case SMOKE_IDLE:
		break;
	}
}
