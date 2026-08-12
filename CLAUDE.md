# zephyr-desktop

A retro (Win95 / Mac System 7 era) desktop shell on Zephyr RTOS + LVGL, with zapps as
dynamically loaded `llext` extensions discovered on a filesystem at runtime.

`docs/architecture.md` is how the desktop works and `docs/abi.md` is the app
contract; read both before changing either. `docs/history.md` is the build log
that milestone letters in comments point at -- provenance, not required reading.

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
   file browser already ran on it. Storage arrived in 0.3, window state in 0.4,
   0.5 is what Notepad needed (keyboard input, an editable text widget, a clipboard,
   menus, dialogs and a clock), and 0.6 is what the file browser needed: a list
   widget, a text-prompt dialog, and one zapp launching another with an argument.
   The browser needed *nothing* new from storage -- every filesystem call it makes
   had been in the ABI, unused, since 0.3.

   0.7 is Minesweeper, and it is the one that was not an application. A game
   asked for a way to draw something the desktop will never have a widget for
   (`zd_grid_t`, a rectangle of bevelled cells) and for the ability to do
   anything at all without being clicked (`timer_start()`, `ZD_EV_TIMER` -- the
   first non-reactive thing in the ABI, missing since 0.1 and never noticed).
   **Pick the next zapp for being structurally unlike what the ABI already
   serves**, not for being the next obvious application; the two candidates
   history.md had named would both have asked for one more widget.

   The append-only rule covers `enum zd_event_type` and the `ZD_KEY_*` constants as
   well as the vtable. New values go on the *end*, never next to the ones they belong
   with: inserting one renumbers everything after it and silently breaks zapps built
   against the older minor, and no version gate would catch it.

   The event *union* is allowed to grow, and `sizeof(struct zd_event)` is deliberately
   not asserted — pinning it would forbid the growth that is safe. What is asserted,
   with `_Static_assert` on `offsetof`, is that nothing already there moves. **Offsets
   are the compatibility rule; size is not.**

## Target

**`qemu_cortex_a53` is the single MVP target.** It is graphical, pointer-driven, and
llext-capable, and it runs natively on macOS. This is not a compromise between two
targets — it removes the need for two.

- Display: `zephyr,display = &ramfb0` (`drivers/display/display_qemu_ramfb.c`)
- Pointer: `zephyr,touch = &virtio_input0` (`drivers/input/input_virtio.c`); `board.cmake`
  auto-adds `-device virtio-tablet-device`
- Keyboard: a second `virtio,input` node on `virtio_mmio4` in the app's overlay, plus a
  `-device virtio-keyboard-device` appended to `QEMU_EXTRA_FLAGS` *above*
  `find_package(Zephyr)` in `app/CMakeLists.txt`. The comment there explains why the two
  tidier ways of adding it do not work
- macOS window: `cmake/emu/qemu.cmake` selects the `cocoa` display backend on Apple hosts
- llext: arm64 is llext-capable and `qemu_cortex_a53` is upstream's own
  `integration_platform` for `llext.readonly_mmu`

`native_sim` **cannot** load extensions and is not a target here. `arch/posix/` has no
`elf.c`; `arch_elf_relocate*` are `__weak` stubs returning `-ENOTSUP`, so it builds fine
with `CONFIG_LLEXT=y` and then fails every `llext_load()` at runtime. Do not reintroduce
it "just for iteration" — that forks the zapp model, which is the one thing this project
must not do.

Hardware targets:

- `mimxrt1060_evk/mimxrt1062/qspi` — headless (no panel yet), FAT on SD, and
  **never run on silicon**. `CONFIG_LV_USE_PXP` is the eventual 2D-accel
  experiment.
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
  src/chrome/       retro bevels, titlebar, palette, the shared row list the
                    ABI's list widget and the file picker are both built on,
                    and cellgrid.c -- one lv_obj_t that draws a whole board
  src/shell/        background, taskbar, launcher, clock, window list
  src/host/         host-API vtable, fs shim + zapp storage, session, and the
                    handle/ownership layers over each widget (text, list, grid)
                    plus timer_api.c, the only thing here that is not reactive
  src/loader/       llext discover/load/instance/unload, boot seeding
  src/input/        the one funnel every key enters through, and the US keymap
  src/chrome/       ...also menu bars and their drop-downs
zapps/              desktop apps. Named zapps/, not apps/, so it is never misread
                    as Zephyr's app/. Several files each since ARM moved to
                    LLEXT_TYPE_ELF_RELOCATABLE; zapps/lib/ is the no-libc helpers
                    they share. hello stays one file on purpose. files/ is the
                    browser, and the only ZD_ZAPP_FLAG_SINGLETON zapp. mines/
                    is Minesweeper, and the reason 0.7 exists.
zephyr/ modules/    west-managed, gitignored
```

This directory is both the west topdir and the project git repo.

## Build and run

```sh
west build -b qemu_cortex_a53 app
west build -t run                                     # opens a cocoa window
```

### Checking it without a hand

`west build -t run` opens a window and offers no way in but a mouse, which is how
milestone J came to hand-test a stale binary twice.

**`tools/ci-check.sh` is all of it in one command** — three boards built, both QEMU
images run, every `.llext`'s imports checked, the Kconfig warnings read per board,
and an exit status that is the number of failed checks. Run it before believing a
milestone is done. `.github/workflows/build.yml` is a thin caller, so anything that
fails in CI reproduces here with one line. The pieces, for when you want just one:

```sh
# Drive the image headless: click, type, assert on the console.
tools/qemu-drive.py -d build 'click:30,258' 'wait:1' 'type:hello' 'key:ctrl-s'

# Launch every zapp, close every window, prove the counters came back.
west build -b qemu_cortex_a53 app -- -DEXTRA_CONF_FILE=smoke.conf   # CONFIG_ZD_SMOKE_TEST=y

ZD_CI_BOARDS=qemu ZD_CI_PRISTINE=0 tools/ci-check.sh   # the fast inner loop
```

Read build output **unfiltered**. A `grep` for `error` hid a failure in milestone J and
QEMU then happily ran the previous ELF and reported a false pass.

**Read the Kconfig warnings too, and read them per board.** `CONFIG_LLEXT_HEAP_SIZE`
does not exist on the CoreS3 — ESP32-S3 is Harvard, so the heap is two heaps —
and setting it in `app/prj.conf` did nothing there for a whole milestone while
Kconfig warned on every single build. Same shape as `[H]`'s board fragment whose
`=n` was overridden by a `select`. A knob that does not exist on a target is a knob
that silently does nothing, and the build does tell you. It now lives in the two ARM
board fragments and `ci-check.sh` fails on the warning.

**Note how that warning is worded, because the obvious grep does not match it.**
It reads `was assigned the value '384' but got` / `the value ''. Check these
unsatisfied dependencies: (!HARVARD) (=n).` — *wrapped*, at about 100 columns, so
a search for the whole phrase silently finds nothing. The first version of the CI
check did exactly that and passed on a build that was warning. Match `was assigned
the value` and read the next two lines.

And read the smoke test's *reasons*, not its counts. `badabi` is last in the seed
order, so when the llext heap stops holding one more zapp it is the entry that is
supposed to be refused that fails to load — with `-ENOMEM`, while the summary
still says "1 refused" and PASSes, and the ABI version gate has quietly stopped
being exercised. That happened in M, one milestone after K wrote down that it
would. `smoke.c` now checks the errno itself; the general lesson is that a
prediction written in a document is not a check.

## Rules that are easy to get wrong

- **Never destroy during dispatch.** A zapp calling `window_close()` from its own callback
  is inside `lv_timer_handler()` on a stack frame owned by the extension. Deleting the
  LVGL subtree there — let alone `llext_unload()` — is a use-after-free. Everything goes
  through the deferred reap in `app/src/wm/`. This is the most likely source of faults in
  the whole project, and it now has four customers, only one of which has a zapp in it:

  - **The taskbar window list.** Clicking one of its buttons changes focus, which fires
    `wm->on_client_list_changed`, which would `lv_obj_clean()` the row holding the button
    currently dispatching. `shell/tasklist.c` sets a dirty flag; `zd_tasklist_reap()`
    rebuilds from the loop.
  - **Menu drop-downs.** Choosing File → Exit fires `ZD_EV_MENU`, the zapp closes its
    window, and that happens on a frame standing on the row that was clicked.
    `zd_menu_close()` only hides; `zd_menu_reap()` empties.
  - **Dialogs, at BOTH ends.** Answering is the obvious half. The other half is that the
    answer to one dialog is very often *another dialog* — "save it?" → Yes → "save as
    what?" — and building that inline would clean the panel holding the Yes button.
    **Opening is deferred too.** Generalise from this: any surface rebuilt *in response
    to* an event needs the treatment, not just one destroyed by an event.

  - **A list widget's rows**, which is the fifth and the one that stopped being
    ad-hoc. A zapp answers `ZD_EV_LIST_ACTIVATE` by emptying and refilling the
    very list whose row is dispatching. Rather than a sixth flag, `chrome/rowlist.c`
    keeps a **model** the zapp mutates and a view the loop re-derives — which is
    the same rule the WM already applies to z-order, and it makes the safety
    structural rather than something every caller has to remember. Getters answer
    from the model immediately, so nobody has to reason about when the pixels
    catch up. Generalise from *this* one: a surface that is rebuilt often enough
    wants a model, not another deferral.

  There is no sixth, and that is the point of `chrome/cellgrid.c`. A zapp
  rewrites a whole Minesweeper board from inside the dispatch of a click on it —
  the same hazard again — and it is safe because a grid is **one** `lv_obj_t`
  that draws every cell itself. No children, so nothing exists to be destroyed
  under the callback: no reap, no dirty flag, no deferral. Milestone M added a
  widget and added nothing to the drain order below. **Take this as a direction,
  not a curiosity: when a surface is rebuilt often enough, drawing it yourself is
  both cheaper and safer than composing it out of widgets**, and the deferral
  machinery is what composing costs.

  Everything deferred is drained from `main()`'s loop in a fixed order — windows,
  instances, queued launches, taskbar, menu, dialog, lists, then queued keys — and
  the order is load-bearing. Lists come after dialogs specifically, because
  `zd_dialog_reap()` builds the file picker and fills its model during that call.
- **Keys are queued, and touching LVGL off the desktop thread is the bug the queue
  prevents.** A key arrives on Zephyr's input thread; everything downstream of routing it
  — the text widget, a dialog's field, a zapp's `event()` — is LVGL's, and only the
  desktop loop may touch that. The first version called straight through and died in two
  keystrokes with `ZEPHYR FATAL ERROR 2: Stack overflow ... thread: input`. The stack was
  the symptom; the locking was the fault. `input/keys.c` queues, `zd_keys_pump()` drains
  from the loop — which is exactly what LVGL's own pointer driver does, so the pointer
  never needed anyone to think about it. **A new input modality inherits the queue.**
- **Hit slop does not compose.** `lv_obj_set_ext_click_area()` is for an *isolated*
  control — the Start button, where the space around it is dead. Adjacent controls must
  instead be *bigger*: LVGL awards an overlap to the last-added child, so two 14 px
  titlebar buttons 2 px apart with `CONFIG_ZD_TOUCH_SLOP_PX=12` send every tap to the
  right-hand one. That bug has now been shipped twice (the launcher menu in E, the
  minimise box in J). Chrome sizes scale with the slop instead — `ZD_BTN_SZ`,
  `ZD_GRIP_SZ`, `ZD_TITLEBAR_H` in `app/src/wm/wm.h` — and a boot selftest asserts the
  rectangles do not overlap on the target, because the value that breaks it lives in a
  board fragment. Met a third time in K with menu bars; `chrome/menu.c` sizes titles and
  rows from the slop, and the on-screen keyboard sidesteps it entirely by being one
  `lv_buttonmatrix`, whose grid cannot overlap by construction.

  A grid cell is the most adjacent control there can be — cells share edges — so it
  is `16 + CONFIG_ZD_TOUCH_SLOP_PX`, and the number is **not exported to zapps to
  multiply**. They ask `grid_fit()` and `grid_measure()`, which is how Minesweeper
  comes up 9x9 on a 480x272 panel and 9x3 on a 320x240 one without knowing either
  number. On the CoreS3 that makes it a demo rather than a game; say so, rather
  than shrinking the cell.
- **A boot check must leave the desktop as it found it.** `test_text()` creates a
  text widget, which takes the caret, which raises the on-screen keyboard on any
  board with `CONFIG_ZD_OSK_AUTO` — and `zd_osk_wanted()` only ever raises, on
  purpose. So the CoreS3 booted with a keyboard across half a 240 px screen, put
  there by a test, for a whole milestone, and QEMU could not show it because slop
  0 means no `OSK_AUTO`. `zd_selftest_run_wm()` now saves and restores it and
  asserts that it did.

  What found it is the cheapest CoreS3 check there is, and is worth reaching for
  before believing anything about that board's layout — build the QEMU image with
  the hardware's geometry:

  ```sh
  # a scratch overlay holding: &ramfb0 { width = <320>; height = <240>; };
  west build -p -b qemu_cortex_a53 -d build-thumb app -- \
      -DEXTRA_DTC_OVERLAY_FILE=$SCRATCH/thumb.overlay -DCONFIG_ZD_TOUCH_SLOP_PX=12
  tools/qemu-drive.py -d build-thumb --width 320 --height 240 'wait:2' 'click:20,226'
  ```

  It has no SD card, no bus arbiter and no real finger, so it is not the board. It
  reproduces every slop and layout decision exactly, in seconds, with a console.
  Note the `--width/--height`: the driver defaults to 480x272 and silently sends
  the wrong coordinates otherwise.
- **Never read a size back off LVGL to compute the next thing's position.**
  `lv_obj_set_size()` does not take effect until a layout pass, so
  `lv_obj_get_width()` right after it answers the *default* -- 130 px for a bare
  `lv_obj`. Milestone K spaced every menu bar that way, which gave a dead gap
  between File and Edit and put the second title off the end of any window
  narrower than about 260 px, where it cannot be clicked at all. It survived a
  whole milestone and a boot check, because that check asserted only that
  adjacent titles do not *overlap* -- and two titles a hundred pixels apart do
  not. Keep the value you set (`struct menu::title_w`); the model is the truth
  here as everywhere else. Where a read-back is genuinely needed,
  `lv_obj_update_layout()` first -- see `label_width()` in the same file.
- **The WM's `sys_dlist_t` is the truth for z-order**, not LVGL's child order. LVGL is a
  projection, re-applied by `zd_wm_restack()` using `lv_obj_move_to_index()`. Note
  `lv_obj_move_foreground()` exists only in LVGL's v8 compatibility shim
  (`api_map/lv_api_map_v8.h`) — don't reach for it.
- **Zapps never see an `lv_obj_t`.** They get opaque handles validated through a generation
  -counted registry with an owner check. The `unsafe_lvgl_content` vtable slot is NULL
  unless the zapp manifest is flagged trusted.
- **The overlay is the fourth layer and everything modal lives on it.** `layers->overlay`
  is the last child of the screen, so it is above the taskbar as well as above every
  window: the Start menu, menu drop-downs, the on-screen keyboard, and dialogs with the
  transparent clickable shade that makes them modal *by construction* rather than by
  everyone agreeing to behave. Anything that must draw over a window and is not a window
  belongs here rather than reparenting itself onto the screen.
- **A zapp must not read `CONFIG_*`.** Zapps compile with `-imacros autoconf.h`, so
  `CONFIG_ZD_TEXT_MAX` is right there and works — and using it welds the zapp to one
  desktop build, which is the thing `include/zd/` exists to prevent. Ask through the ABI
  (`text_get_capacity()`, `window_get_content_size()`) or carry your own constant.
- **Every host-API call takes a `zd_zapp_ctx_t`.** No global "current user". This is what
  makes multi-user a login screen rather than a refactor, and what lets these calls become
  syscalls if `CONFIG_USERSPACE` ever arrives.
- **No direct framebuffer access, anywhere.** Everything stays behind LVGL's draw layer so
  the PXP draw unit can be switched on later without touching WM logic.
- **Zapps have no libc.** `CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n` leaves exactly one
  importable symbol, so `strlen` and `snprintf` are out — and, less obviously, GCC
  synthesises a `memset` call from an ordinary struct assignment. That builds fine and
  fails at load. After touching a zapp, check `nm -u build/<name>.llext` — expect at most
  `zd_get_host_api`. The helpers in `zapps/lib/zapplib.c` exist to keep it that way and
  write through `volatile` pointers precisely so the optimiser cannot undo them.

  On Xtensa the check is `nm -D -u` (the artifact is a shared object, so the ordinary
  symbol table is empty) and it reports an undefined `memset` that the ARM builds do not.
  That is pre-existing, present for `hello` as much as anything else, and does not stop
  the board loading zapps — but it means the ARM builds are the ones this check is
  actually sharp on.

  **Run it on the Xtensa build too, though, because the trap is not only
  `memset`.** 32-bit Xtensa has no 64-bit divide instruction, so
  `(uptime_ms() - started) / 1000` — one instruction on arm64 — becomes a call
  to libgcc's `__divdi3`, which builds cleanly and fails at load on the one board
  this project has run on silicon. Narrow to `uint32_t` before dividing. Same
  family as the synthesised `memset`: the compiler emitting a call nobody wrote,
  and this one exists on exactly one architecture.
- **Permissions are advisory.** The fs shim is a contract, not a security boundary: with
  no MMU isolation a loaded llext is trusted code in the kernel address space. Say so in
  docs rather than implying otherwise.
