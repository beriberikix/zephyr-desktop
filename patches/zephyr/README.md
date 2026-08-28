# Zephyr patches

Applied by `tools/zephyr-patches.sh` (`--drop` to revert). **`west update` resets
`zephyr/` and deletes them — rerun the script after every update.**

## The base moved

These no longer sit on the pin in `manifest/west.yml`. They sit on
**[zephyr#117658](https://github.com/zephyrproject-rtos/zephyr/pull/117658)**,
which the script fetches directly.

That PR is Sylvio Alves' draft, opened 2026-08-27. It carries the ESP32-P4 MIPI
DSI stack, an indexed framebuffer display API, a PPA driver and an LVGL draw
unit for it. This project had been carrying its own version of nearly all of
that — an indexed framebuffer API, LVGL rendering into driver framebuffers, and
a PPA fill driver — and his is more complete: blend and scale-rotate-mirror as
well as fill. So we track his work and keep only what is genuinely ours.

The superseded commits are still reachable on the `zd-p4-dsi-old` branch in
`zephyr/`, including a PPA fill driver that measured **6.4x faster than the CPU**
on a full-screen fill (36.5 ms → 5.7 ms, 84 → 539 MB/s, cache maintenance
counted on both sides). That measurement is worth keeping; the code it came
from is not.

The manifest pin is now only what `west update` restores before the script
fetches over it. Do not read it as the version this builds against.

## What is left, and why

| Patch | Why it is ours |
| --- | --- |
| 0001 | `jadard,jd9365` panel driver — the Waveshare 10.1in panel. Upstream has EK79007 only. |
| 0002 | GT911 `irq-gpios` made optional. Our panel's FPC has no interrupt line, and `GT911_INIT` reaches for the pin unconditionally, so the driver cannot be instantiated without it. |
| 0003 | The `esp32p4_module_dev_kit` board port. |
| 0004 | **v1.3 silicon**: the D-PHY PLL reference. |
| 0005 | **v1.3 silicon**: the DW-GDMA bus-clock race. |
| 0006 | Enables the PPA on the board and configures the LVGL demo for this panel and this silicon. |

0004 and 0005 are not board-specific and are not in upstream's tree — verified
against the rebased #116548 and against #117658. Both are reported upstream at
[#116548 (comment)](https://github.com/zephyrproject-rtos/zephyr/pull/116548#issuecomment-5445236569):

- `dsi_esp32.c` asks for the v3.0+ D-PHY PLL reference (XTAL) unconditionally.
  Before v3.0 the reference is PLL_F20M through a different register, and that
  LL variant's switch ends in `abort()`. On v1.3 the compiler folds it, so
  `mipi_dsi_esp32_init()` is a few register writes and then an unconditional
  jump to `abort()` — a kernel panic before anything prints. This is not only
  our out-of-tree board: `esp32p4_function_ev_board` selects
  `SOC_ESP32P4_REV_1_3` too.
- `display_esp32_dsi.c` enables the DW-GDMA bus clock and resets the controller
  on the next line. Those are writes to two different peripherals and the reset
  faults if it lands first. Nothing in the driver decides which happens: the
  display sample ran while the LVGL demo faulted at the same instruction with a
  byte-identical Kconfig set *and byte-identical generated code*, and switching
  that build to `CONFIG_LOG_MODE_IMMEDIATE` made it run.

## Verified on this base

- `samples/drivers/display` shows correct colour bars on the P4, with the panel
  reporting `JD9365 panel initialized (800x1280, 2 lanes)`.
- `ZD_CI_BOARDS=qemu tools/ci-check.sh` → 8 checks, 0 failed. Worth noting
  because #117658's base is about a month newer Zephyr than the manifest pin,
  so this is what says the rest of the project survived the jump.

## The L2 cache on v1.3

0006 carries the one thing in the EV board's fragment that cannot be copied.
On v1.3 the L2 cache is carved off the **top** of SRAM with the floor fixed at
`0x4ff40000`, so usable SRAM is `0x4ffc0000 - cache - 0x4ff40000`:

| L2 cache | usable SRAM on v1.3 |
| --- | --- |
| 128 KB (default) | 384 KB |
| 256 KB | 256 KB |
| 512 KB (what the EV board uses) | **0 KB** |

The v3.x branch anchors the cache at the bottom instead and has 768 KB to
spend. So 256 KB is the ceiling here, not a preference, and it brings 64-byte
cache lines with it — hence `LV_DRAW_BUF_ALIGN=64` where the EV board sets 128.
