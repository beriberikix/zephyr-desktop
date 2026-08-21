/*
 * zephyr-desktop — HTTP for zapps, off the desktop thread.
 *
 * See http_api.h for why this exists and why it looks like input/keys.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zd/zapp_abi.h>

#include "http_api.h"
#include "../loader/zapp_instance.h"

LOG_MODULE_REGISTER(zd_http, CONFIG_ZD_LOG_LEVEL);

#ifdef CONFIG_ZD_NET
#include <zephyr/net/socket.h>
#endif

#define SLOTS    CONFIG_ZD_HTTP_MAX_REQUESTS
#define BODY_CAP CONFIG_ZD_HTTP_BODY_MAX

enum slot_state {
	SLOT_FREE = 0,
	SLOT_IN_FLIGHT, /* handed to the worker; only the worker may write body */
	SLOT_DONE,      /* worker finished; only the desktop thread touches it */
};

struct slot {
	enum slot_state state;
	/* Compared, never dereferenced, by the worker. Ownership is resolved on
	 * the desktop thread at pump time.
	 */
	struct zd_zapp_instance *owner;
	uint16_t id;
	int16_t status;
	uint32_t len;
	bool delivered; /* event dispatched; body readable until released */
	char host[64];
	char path[192];
	uint16_t port;
	char body[BODY_CAP];
	uint32_t req_len;
};

static struct slot slots[SLOTS];
static uint16_t next_id = 1;

/* The worker owns a slot index while it works on it. Nothing else writes to
 * that slot's body in the meantime, which is what makes this safe without a
 * lock around the buffer itself.
 */
K_MSGQ_DEFINE(work_queue, sizeof(uint8_t), SLOTS, 1);
K_MSGQ_DEFINE(done_queue, sizeof(uint8_t), SLOTS, 1);

/* Guards the slot table's bookkeeping fields, not the body buffers. */
K_MUTEX_DEFINE(slots_lock);

/* ── URL parsing ─────────────────────────────────────────────────────── */

/**
 * http://host[:port]/path
 *
 * https:// is rejected rather than downgraded. The desktop has no TLS stack,
 * and quietly sending a request in the clear that the caller asked to be
 * encrypted is the kind of helpfulness that ends up in an advisory.
 */
static int parse_url(const char *url, char *host, size_t host_cap, uint16_t *port,
		     char *path, size_t path_cap)
{
	static const char scheme[] = "http://";
	const char *cursor;
	size_t n = 0;

	if (url == NULL) {
		return -EINVAL;
	}
	if (strncmp(url, "https://", 8) == 0) {
		LOG_ERR("https is not supported; the desktop has no TLS stack");
		return -EINVAL;
	}
	if (strncmp(url, scheme, sizeof(scheme) - 1) != 0) {
		return -EINVAL;
	}

	cursor = url + sizeof(scheme) - 1;
	*port = 80;

	while (*cursor != '\0' && *cursor != '/' && *cursor != ':') {
		if (n + 1 >= host_cap) {
			return -EINVAL;
		}
		host[n++] = *cursor++;
	}
	host[n] = '\0';
	if (n == 0) {
		return -EINVAL;
	}

	if (*cursor == ':') {
		uint32_t value = 0;

		cursor++;
		while (*cursor >= '0' && *cursor <= '9') {
			value = value * 10U + (uint32_t)(*cursor++ - '0');
			if (value > 65535U) {
				return -EINVAL;
			}
		}
		if (value == 0U) {
			return -EINVAL;
		}
		*port = (uint16_t)value;
	}

	if (*cursor == '\0') {
		cursor = "/";
	}
	if (strlen(cursor) + 1 > path_cap) {
		return -EINVAL;
	}
	strcpy(path, cursor);

	return 0;
}

/* ── the worker ──────────────────────────────────────────────────────── */

#ifdef CONFIG_ZD_NET

/** Read the whole response, then keep only what follows the header break. */
static int do_request(struct slot *slot)
{
	static const char crlf2[] = "\r\n\r\n";
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *result = NULL;
	char port_text[8];
	char header[320];
	int sock = -1;
	int rc;
	uint32_t total = 0;
	char *split;
	int status = -EIO;

	(void)snprintk(port_text, sizeof(port_text), "%u", slot->port);
	rc = zsock_getaddrinfo(slot->host, port_text, &hints, &result);
	if (rc != 0 || result == NULL) {
		LOG_ERR("getaddrinfo(%s) failed: %d", slot->host, rc);
		return -EHOSTUNREACH;
	}

	sock = zsock_socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (sock < 0) {
		zsock_freeaddrinfo(result);
		return -errno;
	}

	/* The Space answers in tens of seconds. Without a timeout a stalled
	 * connection would hold this worker -- and therefore every queued
	 * request -- indefinitely.
	 */
	struct zsock_timeval tv = {
		.tv_sec = CONFIG_ZD_HTTP_TIMEOUT_S,
		.tv_usec = 0,
	};
	(void)zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (zsock_connect(sock, result->ai_addr, result->ai_addrlen) < 0) {
		status = -ECONNREFUSED;
		goto out;
	}

	if (slot->req_len > 0U) {
		rc = snprintk(header, sizeof(header),
			      "POST %s HTTP/1.1\r\nHost: %s\r\n"
			      "Content-Type: text/plain\r\nContent-Length: %u\r\n"
			      "Connection: close\r\n\r\n",
			      slot->path, slot->host, slot->req_len);
	} else {
		rc = snprintk(header, sizeof(header),
			      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
			      slot->path, slot->host);
	}
	if (rc < 0 || rc >= (int)sizeof(header)) {
		status = -EINVAL;
		goto out;
	}

	if (zsock_send(sock, header, (size_t)rc, 0) < 0) {
		status = -EIO;
		goto out;
	}
	if (slot->req_len > 0U && zsock_send(sock, slot->body, slot->req_len, 0) < 0) {
		status = -EIO;
		goto out;
	}

	/* Reuse the request buffer for the response: the request has been sent
	 * and a slot only ever holds one or the other.
	 */
	while (total + 1U < BODY_CAP) {
		int got = zsock_recv(sock, slot->body + total, BODY_CAP - 1U - total, 0);

		if (got < 0) {
			status = (total > 0U) ? -ETIMEDOUT : -ETIMEDOUT;
			break;
		}
		if (got == 0) {
			status = 0;
			break;
		}
		total += (uint32_t)got;
	}
	slot->body[total] = '\0';

	if (status != 0 && total == 0U) {
		goto out;
	}

	/* "HTTP/1.1 200 OK" -> 200 */
	status = -EPROTO;
	if (total > 12U && strncmp(slot->body, "HTTP/1.", 7) == 0) {
		status = (int)((slot->body[9] - '0') * 100 + (slot->body[10] - '0') * 10 +
			       (slot->body[11] - '0'));
	}

	split = strstr(slot->body, crlf2);
	if (split != NULL) {
		uint32_t offset = (uint32_t)(split - slot->body) + 4U;

		slot->len = total - offset;
		memmove(slot->body, slot->body + offset, slot->len);
		slot->body[slot->len] = '\0';
	} else {
		/* No header break: keep nothing rather than hand a zapp a
		 * response that is mostly headers and call it a body.
		 */
		slot->len = 0;
		slot->body[0] = '\0';
	}

out:
	zsock_close(sock);
	zsock_freeaddrinfo(result);
	return status;
}

static void worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		uint8_t index;

		if (k_msgq_get(&work_queue, &index, K_FOREVER) != 0) {
			continue;
		}

		struct slot *slot = &slots[index];
		int status = do_request(slot);

		k_mutex_lock(&slots_lock, K_FOREVER);
		slot->status = (int16_t)status;
		if (status < 0) {
			slot->len = 0;
		}
		slot->state = SLOT_DONE;
		k_mutex_unlock(&slots_lock);

		/* Hand it to the desktop thread. Never dispatch from here:
		 * everything downstream of a response is LVGL's.
		 */
		(void)k_msgq_put(&done_queue, &index, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(zd_http_worker, CONFIG_ZD_HTTP_STACK_SIZE, worker, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

#endif /* CONFIG_ZD_NET */

/* ── the desktop-thread side ─────────────────────────────────────────── */

int zd_http_request(struct zd_zapp_instance *owner, const char *url, const char *body,
		    uint16_t *out_id)
{
#ifndef CONFIG_ZD_NET
	ARG_UNUSED(owner);
	ARG_UNUSED(url);
	ARG_UNUSED(body);
	ARG_UNUSED(out_id);
	return -ENOSYS;
#else
	struct slot *slot = NULL;
	uint8_t index = 0;
	size_t body_len = (body != NULL) ? strlen(body) : 0U;
	int rc;

	if (owner == NULL || out_id == NULL) {
		return -EINVAL;
	}
	if (body_len + 1U > BODY_CAP) {
		return -EINVAL;
	}

	k_mutex_lock(&slots_lock, K_FOREVER);
	for (uint8_t i = 0; i < SLOTS; i++) {
		if (slots[i].state == SLOT_FREE) {
			slot = &slots[i];
			index = i;
			break;
		}
	}
	if (slot == NULL) {
		k_mutex_unlock(&slots_lock);
		return -EBUSY;
	}

	rc = parse_url(url, slot->host, sizeof(slot->host), &slot->port, slot->path,
		       sizeof(slot->path));
	if (rc != 0) {
		k_mutex_unlock(&slots_lock);
		return rc;
	}

	slot->owner = owner;
	slot->id = next_id++;
	if (next_id == 0U) {
		next_id = 1U; /* 0 is never a valid id */
	}
	slot->status = 0;
	slot->len = 0;
	slot->delivered = false;
	slot->req_len = (uint32_t)body_len;
	if (body_len > 0U) {
		memcpy(slot->body, body, body_len);
	}
	slot->body[body_len] = '\0';
	slot->state = SLOT_IN_FLIGHT;
	*out_id = slot->id;
	k_mutex_unlock(&slots_lock);

	if (k_msgq_put(&work_queue, &index, K_NO_WAIT) != 0) {
		k_mutex_lock(&slots_lock, K_FOREVER);
		slot->state = SLOT_FREE;
		slot->owner = NULL;
		k_mutex_unlock(&slots_lock);
		return -EBUSY;
	}

	return 0;
#endif
}

static struct slot *find_done(struct zd_zapp_instance *owner, uint16_t id)
{
	for (size_t i = 0; i < SLOTS; i++) {
		if (slots[i].state == SLOT_DONE && slots[i].owner == owner &&
		    slots[i].id == id) {
			return &slots[i];
		}
	}

	return NULL;
}

int zd_http_read(struct zd_zapp_instance *owner, uint16_t id, uint32_t from, char *buf,
		 uint32_t cap)
{
	struct slot *slot;
	uint32_t n;

	if (owner == NULL || buf == NULL || cap == 0U) {
		return -EINVAL;
	}

	slot = find_done(owner, id);
	if (slot == NULL || !slot->delivered) {
		return -EINVAL;
	}
	if (from >= slot->len) {
		buf[0] = '\0';
		return 0;
	}

	n = slot->len - from;
	if (n > cap - 1U) {
		n = cap - 1U;
	}
	memcpy(buf, slot->body + from, n);
	buf[n] = '\0';

	return (int)n;
}

void zd_http_release(struct zd_zapp_instance *owner, uint16_t id)
{
	struct slot *slot = find_done(owner, id);

	if (slot == NULL) {
		return;
	}

	k_mutex_lock(&slots_lock, K_FOREVER);
	slot->state = SLOT_FREE;
	slot->owner = NULL;
	slot->len = 0;
	slot->delivered = false;
	k_mutex_unlock(&slots_lock);
}

void zd_http_pump(void)
{
	uint8_t index;

	while (k_msgq_get(&done_queue, &index, K_NO_WAIT) == 0) {
		struct slot *slot = &slots[index];
		struct zd_event ev = {
			.type = ZD_EV_HTTP_RESPONSE,
			/* No window, for the reason ZD_EV_TIMER has none. */
			.win = NULL,
		};

		if (slot->owner == NULL) {
			/* The instance went away while this was in flight.
			 * Dropping it is the point of resolving ownership here
			 * rather than on the worker.
			 */
			slot->state = SLOT_FREE;
			continue;
		}

		slot->delivered = true;
		ev.http.id = slot->id;
		ev.http.status = slot->status;
		ev.http.len = slot->len;
		zd_zapp_dispatch(slot->owner, &ev);

		/* Not released here. The zapp reads the body during or after
		 * its callback and releases when it is done; holding it until
		 * then is the whole reason the body is not in the event.
		 */
	}
}

void zd_http_owner_gone(struct zd_zapp_instance *inst)
{
	k_mutex_lock(&slots_lock, K_FOREVER);
	for (size_t i = 0; i < SLOTS; i++) {
		if (slots[i].owner != inst) {
			continue;
		}
		/* An in-flight slot cannot be freed here: the worker is still
		 * writing to its body. Clearing the owner is enough -- the
		 * worker never dereferences it, and pump() drops a response
		 * with no owner and frees the slot then.
		 */
		slots[i].owner = NULL;
		if (slots[i].state == SLOT_DONE) {
			slots[i].state = SLOT_FREE;
			slots[i].len = 0;
			slots[i].delivered = false;
		}
	}
	k_mutex_unlock(&slots_lock);
}

uint32_t zd_http_live_count(void)
{
	uint32_t live = 0;

	for (size_t i = 0; i < SLOTS; i++) {
		if (slots[i].state != SLOT_FREE) {
			live++;
		}
	}

	return live;
}
