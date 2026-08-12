# How zephyr-desktop works

For contributors, and for anyone curious how a desktop shell fits on a
microcontroller. If you only want to *use* it, [using.md](using.md) is shorter.
If you want to write an app for it, you need [writing-a-zapp.md](writing-a-zapp.md)
and [abi.md](abi.md), and none of this.

Three ideas carry most of the weight, and the rest is detail:

1. **The window manager is a tiny X11 stacking WM.** A client struct per window,
   one linked list that is authoritative for z-order, one dispatch path.
2. **Nothing is destroyed during dispatch.** Everything that frees memory is
   queued and drained from the main loop.
3. **The app ABI is a vtable, not a symbol pile.** The desktop exports exactly
   one symbol; apps get opaque handles, never an `lv_obj_t`.

---

## The layers

Four sibling LVGL containers on the active screen, created once at boot, whose
order is never permuted (`app/src/shell/desktop.c`):

```
screen
├── desktop   patterned background; a click here defocuses everything
├── windows   every client frame; z-order is child order within this layer
├── panel     the taskbar; permanently above every window
└── overlay   the Start menu, menu drop-downs, dialogs, on-screen keyboard
```

Because raising a window only reorders children *within* `windows`, the taskbar
can never be occluded and the background can never be raised. That removes a
whole class of stacking bug for free, and it is the reason the answer to "where
should this new floating thing live?" is nearly always `overlay`.

**The overlay is what makes modality structural rather than conventional.** A
dialog puts a transparent, clickable shade across the whole overlay, so clicks
cannot reach a window beneath it — not because every widget agreed to behave,
but because nothing can get past the shade.

## The client struct

`app/src/wm/wm.h`. One `struct zd_client` per window, allocated from a fixed
`K_MEM_SLAB` rather than the heap: bounded, and a leak shows up immediately as
slab exhaustion rather than as slow growth nobody notices.

The parts that matter:

- **`sys_dnode_t node`** — its place in `wm->stack`, head first. **This list is
  the truth about z-order.** LVGL's child order is a projection, re-applied by
  `zd_wm_restack()`. Never read z-order back out of LVGL.
- **`lv_area_t geom`** — the same rule for geometry. The model is authoritative
  and LVGL follows it.
- **`bool minimized`** — the X11 distinction between mapped and alive. A
  minimised window is still in the stack in the same place, so restoring it
  keeps its z-order for free; only the projection changes.
- **`text_focus` / `list_focus`** — keyboard focus *within* a window. The WM
  does not know what an `lv_textarea` or a row list is; it asks a hook whether
  something consumed the key, and the two fields clear each other so a window
  has exactly one place keys go.
- **`owner`** — the zapp instance, or `NULL` for a desktop-internal window. The
  selftests create windows with no owner, which is why the WM has to tolerate it.

## Dispatch

LVGL supplies the input device and the hit test; the WM supplies the policy.
Two rules keep that from scattering into callbacks everywhere:

**One WM callback per frame.** `LV_OBJ_FLAG_EVENT_BUBBLE` on the children means
a press anywhere inside a window bubbles up to its frame, so *raise and focus*
is one code path no matter what was clicked. The event then carries on to the
widget actually hit.

**Drag is the WM's, not LVGL's.** On titlebar press the WM records the pointer
position and the window's geometry; on `PRESSING` it applies the delta; on
release it clears. Resize from the corner grip is the same path with a
different `drag_mode`. Keeping hit-test → focus → dispatch inside the WM is what
lets a new input modality route identically rather than inheriting whatever a
widget happened to do.

## Nothing is destroyed during dispatch

This is the rule most likely to bite anyone changing the desktop.

A zapp that closes its own window from inside its own event callback is,
transitively, inside `lv_timer_handler()`, on a stack frame that lives in the
extension's text. Deleting the LVGL subtree there — let alone `llext_unload()`
— is a use-after-free with a return address pointing into freed code.

So `window_close()` sets `pending_destroy`, unlinks the client from focus and
the stack, and pushes it onto a reap list. `app/src/main.c` drains everything at
the top of the loop, outside dispatch, **in a fixed order that is load-bearing**:

```
zd_wm_reap()          windows
zd_zapp_reap()        instances — after their windows, so a zapp's objects
                      are gone before the code that made them is unmapped
zd_zapp_launch_reap() queued launches, after teardown frees a slot
zd_tasklist_reap()    the taskbar, from a stack the reap has finished with
zd_menu_reap()        dismissed drop-downs
zd_dialog_reap()      dialogs — which also *build* the file picker
zd_rowlist_reap()     lists, after dialogs, since the picker filled a model
zd_keys_pump()        queued keys, last, so one never lands on a dying window
```

Instance teardown additionally waits for `in_zapp_callback` to reach zero — a
depth counter, because a zapp frame anywhere on the stack is enough.

Two generalisations worth carrying, both learned the hard way:

- **Opening is deferred too, not just destroying.** The answer to one dialog is
  very often another dialog ("save it?" → Yes → "save as what?"), and building
  the second inline would clean the panel holding the Yes button. Any surface
  rebuilt *in response to* an event needs the treatment.
- **A surface rebuilt often enough wants a model, not another deferral.** The
  list widget keeps a model the zapp mutates and a view the loop re-derives, so
  getters answer immediately and nobody has to reason about when the pixels
  catch up. That is the same rule the WM already applies to z-order.

And the limit case, which is better than either: **a widget with no children
cannot have its children deleted underneath it.** A cell grid
(`app/src/chrome/cellgrid.c`) is one `lv_obj_t` that draws every cell itself, so
a zapp rewriting a whole Minesweeper board from inside a click *on that board*
is safe by construction — no reap, no dirty flag, nothing added to the list
above. When a surface is redrawn constantly, drawing it yourself is both cheaper
and safer than composing it from widgets.

## Keyboard input, and the thread that must not touch LVGL

A key arrives on Zephyr's input thread. Everything downstream of routing it —
the text widget, a dialog's field, a zapp's `event()` — belongs to LVGL, and
only the desktop loop may touch that. `app/src/input/keys.c` queues; the loop
drains. The first version called straight through and died in two keystrokes
with a stack overflow on the input thread.

This is exactly what LVGL's own pointer driver already does, which is why the
pointer never needed anyone to think about it — and why **a new input modality
inherits the queue.**

## Handles: what a zapp is allowed to hold

Zapps never see an `lv_obj_t`. They get opaque handles from a
generation-counted registry (`app/src/wm/handle.c`) that records the kind of
object and its owning instance. Every call validates all three, and a stale or
foreign handle returns `-EINVAL` rather than faulting.

The generation counter is what makes reuse safe: a slot recycled for a new
object gets a new generation, so an old handle naming that slot no longer
matches. The layers over each widget kind (`host/text_api.c`, `list_api.c`,
`grid_api.c`) all follow the same shape, and each hangs its teardown on
`LV_EVENT_DELETE` so one path covers zapp-destroys, window-closes and
`lv_obj_clean()`.

## Sessions and permissions

Exactly one session exists at boot, and there is **no global "current user"
anywhere.** Every host-API entry takes a `zd_zapp_ctx_t`; the context finds its
instance, the instance finds its session. That is what makes multi-user a login
screen rather than a refactor, and what lets these calls become syscalls if
`CONFIG_USERSPACE` ever arrives.

`app/src/host/fs_shim.c` is the single choke point: normalise, reject `..`,
match against the session's permitted roots and their read/write mode, then call
Zephyr's `fs_*`. The filesystem a zapp sees is the same on every board; only the
mount table differs:

```
/system/zapps/    *.llext, system-installed      read-only to a zapp
/system/share/    fonts, wallpaper, assets       read-only to a zapp
/home/user/       the session's home             read-write
/home/user/zapps/ user-installed *.llext         read-write, also enumerated
/tmp/             scratch                        read-write
```

**Permissions are advisory.** With no MMU a loaded `.llext` is trusted code in
the kernel address space and can reach any address it can compute. What the
shim buys is that it is the only *linkable* route — the desktop exports one
symbol, so there is no `fs_open()` to call directly — which is the difference
between a contract and a suggestion. [abi.md](abi.md) states the boundary in
full; do not let anything imply more than that.

## Working with LVGL

Five things about LVGL that this project got wrong at least once each. They are
not obvious from the API, and none of them fails loudly.

- **`lv_obj_move_foreground()` only exists in the v8 compatibility shim**
  (`api_map/lv_api_map_v8.h`). Use `lv_obj_move_to_index()`.
- **A size you just set is not readable yet.** `lv_obj_set_size()` takes effect
  at the next layout pass, so `lv_obj_get_width()` immediately after returns the
  *default* — 130 px for a bare `lv_obj`. Keep the value you set; where a
  read-back is genuinely needed, `lv_obj_update_layout()` first.
- **`LV_EVENT_DOUBLE_CLICKED` arrives *before* `LV_EVENT_CLICKED`**, to the same
  object, in the same release. So a double-click handler that rebuilds a list
  runs first and the stale `CLICKED` follows.
- **`LV_EVENT_LONG_PRESSED` fires while the button is still down**, and
  `CLICKED` still follows on release. Same shape as the above: the guard goes in
  the second handler.
- **Hit slop does not compose.** `lv_obj_set_ext_click_area()` is for an
  *isolated* control, where the space around it is dead. Adjacent controls must
  be physically bigger instead, because LVGL awards an overlap to the
  last-added child — two 14 px titlebar buttons 2 px apart with a 12 px slop
  send every tap to the right-hand one. Chrome sizes scale with the slop
  (`ZD_BTN_SZ`, `ZD_GRIP_SZ`, `ZD_TITLEBAR_H` in `app/src/wm/wm.h`) and a boot
  check asserts the rectangles do not overlap on the actual target.

Everything stays behind LVGL's draw layer. **No direct framebuffer access
anywhere**, so a 2D acceleration unit can be switched on later without touching
WM logic.

## The repository

```
manifest/west.yml   the Zephyr pin. This directory exists only so that the west
                    topdir can be the repository root.
include/zd/         the zapp ABI. No Zephyr and no LVGL headers may appear here.
app/                the desktop image (the Zephyr application)
  VERSION           the project's version; the ABI's is in zapp_abi.h
  boards/           one .conf per supported board, plus overlays
  src/wm/           client struct, stacking, focus, drag/resize, handles, keys
  src/chrome/       bevels, titlebar, palette, menu bars, the shared row list,
                    and cellgrid.c — one object that draws a whole board
  src/shell/        background, taskbar, launcher, clock, window list, dialogs,
                    on-screen keyboard
  src/host/         the host-API vtable, the fs shim and zapp storage, session,
                    clipboard, clock, timers, and the handle/ownership layer
                    over each widget kind
  src/loader/       llext discover / load / instance / unload, boot seeding
  src/input/        the one funnel every key enters through, and the US keymap
  src/selftest.c    the assertions that run at every boot
  src/smoke.c       launch-everything, close-everything, check the counters
zapps/              hello, notes, notepad, files, mines — and badabi, which
                    exists only to be refused. lib/ holds the no-libc helpers.
tools/              headless drive, screenshot, zoom, serial log, CI, release
docs/               this and its siblings
```

`zapps/`, not `apps/`: Zephyr's convention is that `app/` holds the application
source, and this project has one. A sibling `apps/` reads as a typo for it every
single time.

## How a zapp gets onto the filesystem

The desktop discovers zapps by scanning a directory at runtime; it does not have
a compiled-in list of them. To make that real without an installer, the build
embeds each `.llext` in the image and `zd_seed_install()` writes any that are
missing to `/system/zapps/` at boot. Delivery is synthetic; discovery, loading
and unloading are not. Delete one from the card and copy your own in, and the
desktop finds yours.

## What is deliberately not here

Named so they do not creep in: further catalog apps (terminal, image viewer,
media player); hardware acceleration; maximise, window snapping, and resize from
any edge but the corner; notifications; multiple displays; hardware-enforced
isolation (`CONFIG_USERSPACE` + memory domains); a theming engine — one palette
is hardcoded; sound; real multi-user login; a thread per zapp
(`ZD_ZAPP_FLAG_WANTS_THREAD` is reserved and not honoured); out-of-tree zapp
builds via the llext EDK; app icons; `native_sim`.

Also absent, each because something specific is missing rather than by taste:

- **An end-task.** A zapp can decline a close, which is what makes "save
  changes?" mean anything, and that means a zapp can hold a window. The
  taskbar's window list is where a force-quit belongs.
- **Undo**, and find/replace. `lv_textarea` has no undo stack.
- **A real clock and a date.** `clock_now()` reports the desktop's own fiction
  on boards with no RTC, and there is no date in the ABI at all. That is why
  Notepad has no Time/Date, why the file browser has no Modified column, and
  why Minesweeper has no high score table — the same gap, three times.
- **File associations.** The browser hardcodes `.txt` → Notepad and says so in
  its own source. A registry is the honest version and wants somewhere to live.
- **Change notification.** Nothing tells anyone that a directory changed, so the
  browser will not notice a file Notepad just saved into the folder on screen.
- **A keyboard layout that is not US.**

## Where the rest is

- [abi.md](abi.md) — the contract, its versioning rules, and what it refuses to
  promise.
- [hardware.md](hardware.md) — the boards, and what porting to another costs.
- [history.md](history.md) — how this was built, milestone by milestone,
  including everything the plan got wrong. Not required reading; it is where the
  milestone letters in `app/src/` comments point.
