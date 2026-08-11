/*
 * zephyr-desktop — handle table.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "handle.h"

LOG_MODULE_DECLARE(zd_main, CONFIG_ZD_LOG_LEVEL);

/* Enough for every window plus a handful of widgets each. */
#define HANDLE_SLOTS (CONFIG_ZD_MAX_CLIENTS * 8)

/*
 * A handle packs a 1-based slot index in the low bits and a generation counter
 * above it. Zero is never valid, so a zeroed struct in a zapp is a dead handle
 * rather than a reference to slot 0.
 */
#define INDEX_BITS 12
#define INDEX_MASK ((1u << INDEX_BITS) - 1u)

BUILD_ASSERT(HANDLE_SLOTS < INDEX_MASK, "handle table larger than the index field");

struct slot {
	void *object;
	struct zd_zapp_instance *owner;
	uint32_t generation;
	uint8_t kind;
	bool live;
};

static struct slot slots[HANDLE_SLOTS];
static uint32_t live_count;

static uintptr_t pack(uint32_t index, uint32_t generation)
{
	return ((uintptr_t)generation << INDEX_BITS) | (uintptr_t)(index + 1u);
}

static struct slot *unpack(uintptr_t handle, uint32_t *generation_out)
{
	uint32_t index = (uint32_t)(handle & INDEX_MASK);

	if (index == 0u || index > HANDLE_SLOTS) {
		return NULL;
	}

	*generation_out = (uint32_t)(handle >> INDEX_BITS);
	return &slots[index - 1u];
}

uintptr_t zd_handle_alloc(enum zd_handle_kind kind, void *object,
			  struct zd_zapp_instance *owner)
{
	for (uint32_t i = 0; i < HANDLE_SLOTS; i++) {
		struct slot *slot = &slots[i];

		if (slot->live) {
			continue;
		}

		slot->object = object;
		slot->owner = owner;
		slot->kind = (uint8_t)kind;
		slot->live = true;
		live_count++;
		return pack(i, slot->generation);
	}

	LOG_ERR("handle table exhausted (%d slots)", HANDLE_SLOTS);
	return 0;
}

void *zd_handle_deref(uintptr_t handle, enum zd_handle_kind kind,
		      struct zd_zapp_instance *owner)
{
	uint32_t generation;
	struct slot *slot = unpack(handle, &generation);

	if (slot == NULL || !slot->live) {
		return NULL;
	}

	/* A handle used after its object was freed: the slot may have been
	 * reused, but the generation will not match.
	 */
	if (slot->generation != generation || slot->kind != (uint8_t)kind) {
		return NULL;
	}

	/* Desktop-internal callers pass NULL and skip the check; a zapp may only
	 * ever touch what it owns.
	 */
	if (owner != NULL && slot->owner != owner) {
		LOG_WRN("instance %p tried to use a handle owned by %p", (void *)owner,
			(void *)slot->owner);
		return NULL;
	}

	return slot->object;
}

void zd_handle_free(uintptr_t handle)
{
	uint32_t generation;
	struct slot *slot = unpack(handle, &generation);

	if (slot == NULL || !slot->live || slot->generation != generation) {
		return;
	}

	slot->live = false;
	slot->object = NULL;
	slot->owner = NULL;
	/* Advance before reuse so the old handle can never resolve again. */
	slot->generation++;
	live_count--;
}

void zd_handle_free_all(struct zd_zapp_instance *owner)
{
	for (uint32_t i = 0; i < HANDLE_SLOTS; i++) {
		struct slot *slot = &slots[i];

		if (slot->live && slot->owner == owner) {
			slot->live = false;
			slot->object = NULL;
			slot->owner = NULL;
			slot->generation++;
			live_count--;
		}
	}
}

uint32_t zd_handle_live_count(void)
{
	return live_count;
}
