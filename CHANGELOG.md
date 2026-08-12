# Changelog

Notable changes per release. The long form — including everything the plan got
wrong and what the build taught us instead — is the milestone log in
[docs/design.md](docs/design.md); this file is the short version, organised by
what changed rather than by what was learned.

Versions are the *desktop's*. The **zapp ABI has its own number**, currently
0.7, and the two move independently: the ABI is what a `.llext` is compiled
against and is governed by the append-only rules in [docs/abi.md](docs/abi.md).

## Unreleased

- The build-identity comment in `app/CMakeLists.txt` overstated what
  `CMAKE_CONFIGURE_DEPENDS` covers. Creating a tag touches neither `HEAD` nor
  the index, so an incremental build immediately after `git tag` keeps the
  previous `git describe` string. Release builds are unaffected —
  `mkrelease.sh` passes `-p always` — and the comment now says so instead of
  implying the string can never be stale.

## v0.1.0 — 2026-08-12

The first release. Thirteen milestones, lettered A to M, got here; this file
exists so the next one has something to be a change from.

### The desktop

- A stacking window manager modelled on a tiny X11 one: a `zd_client` per
  window, one `sys_dlist_t` that is *authoritative* for z-order with LVGL's
  child order as a projection re-applied from it, and one central dispatch path.
- Overlapping draggable windows with hand-built Win95-era chrome — bevels,
  titlebars, focus styling — resizable from the bottom-right grip, minimisable,
  and closable with the zapp allowed to decline.
- A taskbar with a launcher, a clock, and a window list; a fourth "overlay"
  layer above everything for the Start menu, drop-downs, the on-screen keyboard
  and modal dialogs.
- **Nothing is ever destroyed during dispatch.** Window close, instance unload,
  the taskbar's window list, menu drop-downs, dialogs at both ends, and queued
  launches are all drained from the desktop loop in a fixed order. Keyboard
  events are queued off the input thread for the same reason.
- 224 assertions run at every boot, and a `smoke.conf` build that launches every
  zapp, closes every window, and asserts the client, handle and open-file
  counters return to their boot values.

### The zapps

`hello`, `notes`, and then three that each forced the ABI forward:

- **Notepad** — typing, selection, a clipboard, menus, dialogs, open and save.
- **Files** — a browser that navigates the filesystem, makes and deletes
  folders, and hands a `.txt` to Notepad. It needed *nothing* new from storage:
  every filesystem call it makes had been in the ABI, unused, since 0.3.
- **Minesweeper** — the only one that is not an application, and the most useful
  for that reason. It asked for a way to draw something the desktop will never
  have a widget for, and for the ability to do anything at all without being
  clicked.

`badabi` ships alongside them and exists only to be refused: it declares
`ZD_ABI_MAJOR + 1`, and if it ever launches the version gate is broken.

### The zapp ABI, 0.1 → 0.7

One exported symbol, `zd_get_host_api`, against Zephyr's default 172. Zapps see
opaque, generation-counted handles validated against their owner, never an
`lv_obj_t`. Every call takes a `zd_zapp_ctx_t`; there is no global current user.

| | |
|---|---|
| 0.1 | Windows and labels |
| 0.2 | `set_user_data()` / `get_user_data()`, because instances of one zapp share `.bss` |
| 0.3 | Storage: the permission shim, quotas, and a filesystem a zapp may actually reach |
| 0.4 | Window state: resize, minimise, and `ZD_EV_WINDOW_CLOSE_REQUEST` |
| 0.5 | Keyboard input, a text widget, a clipboard, menu bars, dialogs, a clock |
| 0.6 | Lists, a prompt dialog, picker navigation, one zapp launching another, and `ZD_ZAPP_FLAG_SINGLETON` finally enforced |
| 0.7 | Cell grids, timers, a ceiling on `window_set_geometry()`, and `ZD_DLG_OK_ONLY` |

Every one of those is an append. Offsets are the compatibility rule and
`sizeof(struct zd_event)` is deliberately not pinned, so the event union can
grow without moving anything a compiled zapp already reads.

### Targets

- **`qemu_cortex_a53`** — the development target, graphical and pointer-driven,
  running natively on macOS through QEMU's cocoa backend.
- **`m5stack_cores3/esp32s3/procpu`** — 320×240 touchscreen, Xtensa, microSD.
  **The whole thing has run on this board, by thumb.** 619 KB flash.
- **`mimxrt1060_evk/mimxrt1062/qspi`** — builds at 368 KB, headless, and has
  never been run on silicon.

`native_sim` is not a target and cannot be: `arch/posix/` has no `elf.c`, so it
builds happily with `CONFIG_LLEXT=y` and fails every `llext_load()` at runtime.

### Release engineering, all new here

- `LICENSE` — the Apache-2.0 text the per-file SPDX tags have always pointed at.
- `app/VERSION`, and a first console line naming the release, the commit and the
  ABI version.
- `tools/ci-check.sh` — every check this project has, on every board, runnable
  by the person who has to fix it. `.github/workflows/build.yml` calls it, and
  is the first time `west init -l manifest && west update` will have been run by
  anyone but its author.
- `tools/mkrelease.sh` — the per-board SD-card tree, which is the part of a
  release a source tag cannot give you.

### Fixed on the way here

- **The RT1060's llext heap was 256 KB**, not the 384 KB its own comment claimed
  parity with — a board fragment beats `prj.conf`, and the comment is what made
  it invisible. At 256 KB the sixth zapp does not load. Never observed, because
  that board is headless and has never been run.
- **`CONFIG_LLEXT_HEAP_SIZE` was set in `prj.conf`**, where it reached a Harvard
  ESP32-S3 that has no such symbol. Kconfig warned on every CoreS3 build for a
  milestone. It now lives in the two ARM board fragments, and `ci-check.sh`
  fails on that warning — which only works because nobody is generating one on
  purpose any more.
- **`CONFIG_STACK_SENTINEL=y` did nothing on the RT1060**, found by that check
  on its first clean run. It depends on `!MPU_STACK_GUARD`, and the Cortex-M7
  has the hardware guard — which is the better of the two, so the board was
  never unprotected, but a global assignment that silently fails on one target
  is the same shape as the bug above. Stack protection is now chosen per board,
  including a note in the RT1060's fragment saying why it has none of its own.
- **The README's "Honest limits" understated the project** by two milestones:
  window resize landed in J and the clipboard in K.
