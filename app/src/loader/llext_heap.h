/*
 * zephyr-desktop — placing the llext heap somewhere other than SRAM.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_LOADER_LLEXT_HEAP_H_
#define ZD_LOADER_LLEXT_HEAP_H_

#ifdef CONFIG_ZD_LLEXT_HEAP_REGION

/* Hand llext its heap. Must run before anything tries to load a zapp, which in
 * practice means before zd_storage_init() -- discovery does not load, but the
 * boot seed and the smoke test both do.
 */
int zd_llext_heap_init(void);

#else

static inline int zd_llext_heap_init(void)
{
	return 0;
}

#endif /* CONFIG_ZD_LLEXT_HEAP_REGION */

#endif /* ZD_LOADER_LLEXT_HEAP_H_ */
