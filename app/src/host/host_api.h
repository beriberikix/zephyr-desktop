/*
 * zephyr-desktop — the host API vtable.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_HOST_HOST_API_H_
#define ZD_HOST_HOST_API_H_

#include <zd/app_abi.h>

struct zd_app_instance;

/**
 * @brief The vtable an instance should be handed at init.
 *
 * Trusted apps get the full table; everyone else gets one whose
 * unsafe_lvgl_content slot is NULL. Handing out a different table, rather than
 * checking a flag inside the accessor, means an untrusted app has no function
 * to call at all.
 */
const struct zd_host_api *zd_host_api_for(struct zd_app_instance *inst);

#endif /* ZD_HOST_HOST_API_H_ */
