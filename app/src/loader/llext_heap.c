/*
 * zephyr-desktop — placing the llext heap somewhere other than SRAM.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/llext/llext.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "llext_heap.h"

LOG_MODULE_REGISTER(zd_llext_heap, CONFIG_ZD_LOG_LEVEL);

/*
 * The heap zapps are loaded into, parked in a named linker region.
 *
 * Two things make this safe to do as a plain static array rather than a
 * runtime allocation. Z_GENERIC_SECTION puts it in the region's own output
 * section, which the boot .bss clear does not walk -- so nothing writes to this
 * memory before the driver backing the region has initialised it, which on the
 * Presto is a POST_KERNEL step that maps 8 MB of PSRAM into XIP space. And
 * llext_heap_init() takes the buffer as-is, so the memory is never touched
 * between link time and the call below.
 *
 * Alignment is 8 because that is what k_heap_init wants of its backing store.
 */
static uint8_t heap_mem[CONFIG_ZD_LLEXT_HEAP_REGION_KB * 1024]
	Z_GENERIC_SECTION(CONFIG_ZD_LLEXT_HEAP_REGION_NAME) __aligned(8);

int zd_llext_heap_init(void)
{
	int ret = llext_heap_init(heap_mem, sizeof(heap_mem));

	if (ret != 0) {
		/* -ENOSYS means the build is not actually in dynamic-heap mode,
		 * which would leave llext using a static heap this board has no
		 * room for. Worth saying plainly: every zapp load would then fail
		 * with -ENOMEM and the desktop would look like it had no apps.
		 */
		LOG_ERR("llext heap init failed (%d); no zapp will load", ret);
		return ret;
	}

	LOG_INF("llext heap: %zu KB at %p (%s)", sizeof(heap_mem) / 1024, (void *)heap_mem,
		CONFIG_ZD_LLEXT_HEAP_REGION_NAME);

	return 0;
}
