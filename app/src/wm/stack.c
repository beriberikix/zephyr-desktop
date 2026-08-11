/*
 * zephyr-desktop — stacking order.
 *
 * wm->stack is authoritative: head is topmost. LVGL's child order is a
 * projection of it, rewritten wholesale by zd_wm_restack(). Deriving the
 * projection from the model, rather than nudging LVGL one object at a time and
 * reading it back, is what makes always-on-top, minimise and per-zapp window
 * groups cheap to add later.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wm.h"

void zd_wm_restack(struct zd_wm *wm)
{
	sys_dnode_t *node = sys_dlist_peek_tail(&wm->stack);
	int32_t index = 0;

	/* Walk bottom-to-top assigning ascending indices. Each move places the
	 * object immediately above the ones already positioned, so the passes
	 * do not fight each other the way a top-down walk would.
	 */
	while (node != NULL) {
		struct zd_client *client = CONTAINER_OF(node, struct zd_client, node);

		lv_obj_move_to_index(client->frame, index++);
		node = sys_dlist_peek_prev(&wm->stack, node);
	}
}

void zd_wm_raise(struct zd_wm *wm, struct zd_client *client)
{
	if (client->pending_destroy) {
		return;
	}

	/* Already on top: nothing to rewrite. */
	if (sys_dlist_peek_head(&wm->stack) == &client->node) {
		return;
	}

	sys_dlist_remove(&client->node);
	sys_dlist_prepend(&wm->stack, &client->node);
	zd_wm_restack(wm);
}

struct zd_client *zd_wm_top(struct zd_wm *wm)
{
	sys_dnode_t *node = sys_dlist_peek_head(&wm->stack);

	return node != NULL ? CONTAINER_OF(node, struct zd_client, node) : NULL;
}
