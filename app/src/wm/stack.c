/*
 * zephyr-desktop — stacking order.
 *
 * wm->stack is authoritative: head is topmost. LVGL's child order is a
 * projection of it, rewritten wholesale by zd_wm_restack(). Deriving the
 * projection from the model, rather than nudging LVGL one object at a time and
 * reading it back, is what makes always-on-top, minimise and per-zapp window
 * groups cheap to add later.
 *
 * Minimise cashed that cheque. A minimised client keeps its node and its index;
 * the only thing that changes is that the projection hides it. Restoring is
 * therefore exact rather than approximate -- the window comes back exactly where
 * it was in the order, because nothing ever removed it from the order.
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

		/* Mapped-ness is part of the projection too, so it is re-applied
		 * from the model here rather than poked at the call sites. A
		 * hidden frame is not hit-tested, which is what makes a
		 * minimised window unclickable without any extra check in the
		 * dispatch path.
		 */
		if (client->minimized) {
			lv_obj_add_flag(client->frame, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(client->frame, LV_OBJ_FLAG_HIDDEN);
		}

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

/*
 * The frontmost window belonging to @p owner, or NULL.
 *
 * Walks the model rather than asking LVGL, like everything else that wants to
 * know about z-order. Used to answer "you are already running": a singleton
 * told to open something has to bring the window the user will type into to the
 * front, and with several open that is the one they were last using -- which is
 * exactly what frontmost means.
 */
struct zd_client *zd_wm_topmost_of(struct zd_wm *wm, const struct zd_zapp_instance *owner)
{
	struct zd_client *client;

	SYS_DLIST_FOR_EACH_CONTAINER(&wm->stack, client, node) {
		if (client->owner == owner && !client->pending_destroy) {
			return client;
		}
	}

	return NULL;
}

struct zd_client *zd_wm_top(struct zd_wm *wm)
{
	sys_dnode_t *node = sys_dlist_peek_head(&wm->stack);

	/* A minimised client is unmapped, not gone: it keeps its node so that
	 * restoring it puts it back exactly where it was. Everything asking for
	 * "the top window" means the top *visible* one, so skip past them.
	 */
	while (node != NULL) {
		struct zd_client *client = CONTAINER_OF(node, struct zd_client, node);

		if (!client->minimized) {
			return client;
		}
		node = sys_dlist_peek_next(&wm->stack, node);
	}

	return NULL;
}
