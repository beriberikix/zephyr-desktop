/*
 * zephyr-desktop — SPI pin arbitration for the M5Stack CoreS3.
 *
 * GPIO35 on this board does two jobs. It is SPI2 MISO for the microSD card
 * (`SPIM2_MISO_GPIO35` in the board's pinctrl) and it is the LCD's data/command
 * line (`dc-gpios = <&gpio1 3>` on the mipi_dbi node, and &gpio1 3 is GPIO35).
 * The display driver configures it as a GPIO output once at init and never lets
 * go, so the card can never drive a reply: CMD8 goes unanswered, CMD58 returns
 * no OCR, and the mount fails with -EIO. Any card, every time. Upstream's answer
 * for the CoreS3 *SE* variant is to disable the display outright.
 *
 * Arduino's M5GFX solves it by flipping the pin's direction on every chip-select
 * change, so the two devices take turns. This is the same trick at a coarser
 * grain, which is all a desktop needs: the panel is idle for the whole of a file
 * read, so the window can be a logical operation rather than a transaction.
 *
 *   acquire   re-apply the SPI controller's pinctrl state -> GPIO35 becomes an
 *             input again, routed to the FSPIQ peripheral signal, output driver
 *             off. This works because SPIM2_MISO_GPIO35 carries an input signal
 *             and no output signal, so pinctrl's ESP32 backend calls
 *             gpio_ll_output_disable() for it -- undoing exactly what the
 *             display driver did.
 *   release   configure it back as a GPIO output. The mipi_dbi driver sets the
 *             D/C level explicitly before every transfer, so the level we
 *             restore does not matter, only the direction.
 *
 * Reapplying the whole state also touches SCLK and MOSI. That is deliberate: it
 * is idempotent, and asking pinctrl for "the bus's pins" rather than naming
 * GPIO35 here keeps the board's devicetree as the single source of truth.
 *
 * Safety comes from the LVGL mutex rather than from a comment. Holding it for
 * the window makes it impossible for anything to drive the panel while the pin
 * is pointed elsewhere, and it is recursive for the desktop thread, which
 * already holds it when a zapp launch runs inside lv_timer_handler(). It also
 * makes the nesting depth safe to touch without a second lock.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>

#include <lvgl_zephyr.h>

#include "bus_arb.h"

#define ZD_DBI_NODE DT_NODELABEL(mipi_dbi)
#define ZD_SPI_NODE DT_PHANDLE(ZD_DBI_NODE, spi_dev)

/* The whole premise: display and storage on one controller. If a board ever
 * gives them separate buses, it does not need this file at all.
 */
BUILD_ASSERT(DT_SAME_NODE(ZD_SPI_NODE, DT_BUS(DT_NODELABEL(sd0))),
	     "the display and the SD card must share an SPI controller");

PINCTRL_DT_DEV_CONFIG_DECLARE(ZD_SPI_NODE);

static const struct pinctrl_dev_config *const spi_pins = PINCTRL_DT_DEV_CONFIG_GET(ZD_SPI_NODE);
static const struct gpio_dt_spec dc = GPIO_DT_SPEC_GET(ZD_DBI_NODE, dc_gpios);

static uint32_t depth;

void zd_bus_storage_acquire(void)
{
	lvgl_lock();

	if (depth++ == 0) {
		(void)pinctrl_apply_state(spi_pins, PINCTRL_STATE_DEFAULT);
	}
}

void zd_bus_storage_release(void)
{
	if (--depth == 0) {
		(void)gpio_pin_configure_dt(&dc, GPIO_OUTPUT_INACTIVE);
	}

	lvgl_unlock();
}
