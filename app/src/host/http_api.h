/*
 * zephyr-desktop — the first thing a zapp can ask for that does not finish
 * on the desktop thread.
 *
 * Everything in the ABI up to 0.7 completes before it returns. A network
 * request cannot: the assistant Space this was built for answers in tens of
 * seconds, and a zapp runs as a callback inside lv_timer_handler(). A call
 * that waited would hold the drawing thread for the whole request and stop the
 * clock, the taskbar and every other window with it.
 *
 * So the work goes to a dedicated worker thread and the answer comes back
 * through a k_msgq that main()'s loop drains, exactly as input/keys.c does for
 * keystrokes. That is not a coincidence: the rule is the same one, and it is
 * the rule this project already learned the hard way. Everything downstream of
 * a response -- the zapp's event() callback, the text widget it writes into --
 * is LVGL's, and only the desktop thread may touch LVGL. A worker that called
 * dispatch directly would be the input-thread stack overflow again, with a
 * longer fuse.
 *
 * Note what the worker does NOT do: it never touches a zd_zapp_instance beyond
 * comparing the pointer it was handed. Ownership is resolved on the desktop
 * thread at pump time, so an instance that exited while a request was in
 * flight is simply not found, and the response is dropped rather than
 * delivered into unmapped extension text.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_HTTP_API_H_
#define ZD_HOST_HTTP_API_H_

#include <stdint.h>

struct zd_zapp_instance;

/**
 * @brief Start a request on behalf of @p owner. Returns immediately.
 *
 * @param url    absolute http:// URL. https:// is rejected rather than
 *               downgraded -- see docs/clippy.md for why the TLS hop is the
 *               proxy's job.
 * @param body   request body for a POST, or NULL for a GET.
 * @param out_id set to the id the response will carry in ev->http.id.
 *
 * @return 0, -EBUSY when every slot is in flight, -EINVAL for a URL this
 *         cannot parse, or -ENOSYS when the build has no networking.
 */
int zd_http_request(struct zd_zapp_instance *owner, const char *url, const char *body,
		    uint16_t *out_id);

/**
 * @brief Copy out part of a completed body.
 *
 * @return bytes written (NUL-terminated when @p cap >= 1), or -EINVAL for an
 *         id that is not a completed, unreleased response belonging to
 *         @p owner.
 */
int zd_http_read(struct zd_zapp_instance *owner, uint16_t id, uint32_t from, char *buf,
		 uint32_t cap);

/** Release the slot and its buffer. Releasing an unknown id is not an error. */
void zd_http_release(struct zd_zapp_instance *owner, uint16_t id);

/**
 * @brief Deliver finished responses. Called from main()'s loop only.
 *
 * Sits with zd_keys_pump() in the drain order and for the same reason: this is
 * where an answer that arrived on another thread becomes an event on this one.
 */
void zd_http_pump(void);

/**
 * @brief Forget everything belonging to @p inst.
 *
 * Beside zd_timer_owner_gone() in the instance teardown. A response that lands
 * after its zapp has gone must not be dispatched, and its buffer must not leak.
 */
void zd_http_owner_gone(struct zd_zapp_instance *inst);

/** Slots in use, for leak assertions in the smoke test. */
uint32_t zd_http_live_count(void);

#endif /* ZD_HOST_HTTP_API_H_ */
