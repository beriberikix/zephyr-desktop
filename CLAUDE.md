# zephyr-desktop

A retro (Win95 / Mac System 7 era) desktop shell on Zephyr RTOS + LVGL, with apps as
dynamically loaded `llext` extensions discovered on a filesystem at runtime.

The design doc and ordered task list is `docs/design.md`. Read it before doing anything.

## The MVP thesis

Prove the *spine*, not the pixels. Success is: boot to a retro desktop; the launcher
enumerates apps found on the filesystem; clicking one loads its `.llext` at runtime; the
app calls the desktop ABI to open a window and draw "hello world"; two instances can be
open, dragged over each other with correct z-order and focus; closing a window unloads the
extension cleanly with no leaks.

Two things are load-bearing and get real care. Everything else may be scrappy:

1. **The WM data model and event loop** (`app/src/wm/`) — modelled on a tiny X11 stacking
   WM. A `zd_client` struct plus one central dispatch path.
2. **The app ABI** (`include/zd/app_abi.h`) — designed as if the terminal, text editor and
   file browser already ran on it; implemented only as far as hello world needs.

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
it "just for iteration" — that forks the app model, which is the one thing this project
must not do.

Hardware target for later (milestone H): `mimxrt1060_evk`, headless at first (no panel
yet), FAT on SD. `CONFIG_LV_USE_PXP` is the eventual 2D-accel experiment.

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
  src/wm/           client struct, stacking, focus, drag, handle registry
  src/chrome/       retro bevels, titlebar, palette
  src/shell/        background, taskbar, launcher, clock
  src/host/         host-API vtable, fs shim, session
  src/loader/       llext discover/load/instance/unload, boot seeding
apps/               one .c file per app (LLEXT_TYPE_ELF_OBJECT allows only one)
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

- **Never destroy during dispatch.** An app calling `window_close()` from its own callback
  is inside `lv_timer_handler()` on a stack frame owned by the extension. Deleting the
  LVGL subtree there — let alone `llext_unload()` — is a use-after-free. Everything goes
  through the deferred reap in `app/src/wm/`. This is the most likely source of faults in
  the whole project.
- **The WM's `sys_dlist_t` is the truth for z-order**, not LVGL's child order. LVGL is a
  projection, re-applied by `zd_wm_restack()` using `lv_obj_move_to_index()`. Note
  `lv_obj_move_foreground()` exists only in LVGL's v8 compatibility shim
  (`api_map/lv_api_map_v8.h`) — don't reach for it.
- **Apps never see an `lv_obj_t`.** They get opaque handles validated through a generation
  -counted registry with an owner check. The `unsafe_lvgl_content` vtable slot is NULL
  unless the app manifest is flagged trusted.
- **Every host-API call takes a `zd_app_ctx_t`.** No global "current user". This is what
  makes multi-user a login screen rather than a refactor, and what lets these calls become
  syscalls if `CONFIG_USERSPACE` ever arrives.
- **No direct framebuffer access, anywhere.** Everything stays behind LVGL's draw layer so
  the PXP draw unit can be switched on later without touching WM logic.
- **Permissions are advisory.** The fs shim is a contract, not a security boundary: with
  no MMU isolation a loaded llext is trusted code in the kernel address space. Say so in
  docs rather than implying otherwise.
