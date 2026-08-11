# zephyr-desktop

A retro (Win95 / Mac System 7 era) desktop shell on Zephyr RTOS + LVGL, with zapps as
dynamically loaded `llext` extensions discovered on a filesystem at runtime.

The design doc and ordered task list is `docs/design.md`. Read it before doing anything.

## The MVP thesis

Prove the *spine*, not the pixels. Success is: boot to a retro desktop; the launcher
enumerates zapps found on the filesystem; clicking one loads its `.llext` at runtime; the
zapp calls the desktop ABI to open a window and draw "hello world"; two instances can be
open, dragged over each other with correct z-order and focus; closing a window unloads the
extension cleanly with no leaks.

Two things are load-bearing and get real care. Everything else may be scrappy:

1. **The WM data model and event loop** (`app/src/wm/`) — modelled on a tiny X11 stacking
   WM. A `zd_client` struct plus one central dispatch path.
2. **The zapp ABI** (`include/zd/zapp_abi.h`) — designed as if the terminal, text editor and
   file browser already ran on it. Implemented as far as hello world needs, plus storage
   (ABI 0.3), which was the one designed-but-missing half, plus window state — resize,
   minimise and the close handshake (ABI 0.4).

   The append-only rule covers `enum zd_event_type` as well as the vtable. New event
   values go on the *end*, never next to the events they belong with: inserting one
   renumbers everything after it and silently breaks zapps built against the older minor,
   and no version gate would catch it.

## Target

**`qemu_cortex_a53` is the single MVP target.** It is graphical, pointer-driven, and
llext-capable, and it runs natively on macOS. This is not a compromise between two
targets — it removes the need for two.

- Display: `zephyr,display = &ramfb0` (`drivers/display/display_qemu_ramfb.c`)
- Pointer: `zephyr,touch = &virtio_input0` (`drivers/input/input_virtio.c`); `board.cmake`
  auto-adds `-device virtio-tablet-device`
- macOS window: `cmake/emu/qemu.cmake` selects the `cocoa` display backend on Apple hosts
- llext: arm64 is llext-capable and `qemu_cortex_a53` is upstream's own
  `integration_platform` for `llext.readonly_mmu`

`native_sim` **cannot** load extensions and is not a target here. `arch/posix/` has no
`elf.c`; `arch_elf_relocate*` are `__weak` stubs returning `-ENOTSUP`, so it builds fine
with `CONFIG_LLEXT=y` and then fails every `llext_load()` at runtime. Do not reintroduce
it "just for iteration" — that forks the zapp model, which is the one thing this project
must not do.

Hardware targets, both building but not yet run on silicon:

- `mimxrt1060_evk/mimxrt1062/qspi` — headless (no panel yet), FAT on SD.
  `CONFIG_LV_USE_PXP` is the eventual 2D-accel experiment.
- `m5stack_cores3/esp32s3/procpu` — 320x240 touchscreen, microSD, the only
  **non-ARM** target, and the one that has actually run the whole MVP on
  silicon. Xtensa cannot stream zapps off the filesystem: it needs writable
  llext storage, which needs a `peek()`-capable loader, and `llext_fs_loader`
  has none. Hence `CONFIG_ZD_ZAPP_LOAD_VIA_BUFFER`. Also Harvard, so
  `LLEXT_HEAP_SIZE` does not exist there.

  PSRAM is on (`CONFIG_ESP_SPIRAM`), holding the LVGL pool and rendering
  buffers via `.lvgl_heap`/`.lvgl_buf`, which Espressif's linker script already
  routes to `.ext_ram.data`. It must stay **`SPIRAM_MODE_QUAD`** — octal claims
  GPIO33-37, and this board drives the LCD and SD on 35/36/37.

  **GPIO35 is both SPI2 MISO and the LCD's D/C line**, so the display driver's
  output claim stops the card answering CMD8 at all — upstream's answer for the
  SE variant is to disable the display. `app/src/host/bus_arb.c` borrows the pin
  back around each filesystem operation instead. Add a filesystem call site on
  this board and it must be bracketed, or it will read from a pin pointed at the
  screen. Zapp-facing storage all goes through `app/src/host/fs_api.c`, which
  brackets every call it makes; keep it that way rather than growing a second
  call site. See `docs/hardware.md`.

## Zephyr version

Pinned in `manifest/west.yml` to a **`main` commit, not a release tag** —
`e201b84b04e4` (2026-07-31). The a53 display/pointer stack landed 2026-06-15, after the
v4.4.1 tag (2026-06-10), so no release contains it. Do not "helpfully" move the pin to a
tag. LVGL resolves to 9.6.0-dev.

## Layout

```
manifest/west.yml   the pin. This dir exists only so topdir can be the repo root.
include/zd/         the app ABI. No Zephyr and no LVGL headers may appear here.
app/                the desktop image (the Zephyr application)
  src/wm/           client struct, stacking, focus, drag/resize, handle registry
  src/chrome/       retro bevels, titlebar, palette
  src/shell/        background, taskbar, launcher, clock, window list
  src/host/         host-API vtable, fs shim + zapp storage, session
  src/loader/       llext discover/load/instance/unload, boot seeding
zapps/              desktop apps, one .c file each (ELF_OBJECT allows only one).
                    Named zapps/, not apps/, so it is never misread as Zephyr's app/
zephyr/ modules/    west-managed, gitignored
```

This directory is both the west topdir and the project git repo.

## Build and run

```sh
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1     # matches zephyr/SDK_VERSION
west build -b qemu_cortex_a53 app
west build -t run                                     # opens a cocoa window
```

## Rules that are easy to get wrong

- **Never destroy during dispatch.** A zapp calling `window_close()` from its own callback
  is inside `lv_timer_handler()` on a stack frame owned by the extension. Deleting the
  LVGL subtree there — let alone `llext_unload()` — is a use-after-free. Everything goes
  through the deferred reap in `app/src/wm/`. This is the most likely source of faults in
  the whole project.

  It has a second customer with no zapp in it: the taskbar window list. Clicking one of
  its buttons changes focus, which fires `wm->on_client_list_changed`, which would
  `lv_obj_clean()` the row holding the button currently dispatching. `shell/tasklist.c`
  therefore only sets a dirty flag; `zd_tasklist_reap()` rebuilds from the desktop loop,
  after `zd_wm_reap()`. Any new shell surface derived from the WM's state needs the same
  treatment.
- **Hit slop does not compose.** `lv_obj_set_ext_click_area()` is for an *isolated*
  control — the Start button, where the space around it is dead. Adjacent controls must
  instead be *bigger*: LVGL awards an overlap to the last-added child, so two 14 px
  titlebar buttons 2 px apart with `CONFIG_ZD_TOUCH_SLOP_PX=12` send every tap to the
  right-hand one. That bug has now been shipped twice (the launcher menu in E, the
  minimise box in J). Chrome sizes scale with the slop instead — `ZD_BTN_SZ`,
  `ZD_GRIP_SZ`, `ZD_TITLEBAR_H` in `app/src/wm/wm.h` — and a boot selftest asserts the
  rectangles do not overlap on the target, because the value that breaks it lives in a
  board fragment.
- **The WM's `sys_dlist_t` is the truth for z-order**, not LVGL's child order. LVGL is a
  projection, re-applied by `zd_wm_restack()` using `lv_obj_move_to_index()`. Note
  `lv_obj_move_foreground()` exists only in LVGL's v8 compatibility shim
  (`api_map/lv_api_map_v8.h`) — don't reach for it.
- **Zapps never see an `lv_obj_t`.** They get opaque handles validated through a generation
  -counted registry with an owner check. The `unsafe_lvgl_content` vtable slot is NULL
  unless the zapp manifest is flagged trusted.
- **Every host-API call takes a `zd_zapp_ctx_t`.** No global "current user". This is what
  makes multi-user a login screen rather than a refactor, and what lets these calls become
  syscalls if `CONFIG_USERSPACE` ever arrives.
- **No direct framebuffer access, anywhere.** Everything stays behind LVGL's draw layer so
  the PXP draw unit can be switched on later without touching WM logic.
- **Zapps have no libc.** `CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n` leaves exactly one
  importable symbol, so `strlen` and `snprintf` are out — and, less obviously, GCC
  synthesises a `memset` call from an ordinary struct assignment. That builds fine and
  fails at load. After touching a zapp, check `nm -u build/<name>.llext`.
- **Permissions are advisory.** The fs shim is a contract, not a security boundary: with
  no MMU isolation a loaded llext is trusted code in the kernel address space. Say so in
  docs rather than implying otherwise.
