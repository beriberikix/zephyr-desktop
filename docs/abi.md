# The zephyr-desktop zapp ABI

The contract is `include/zd/zapp_abi.h`. This document explains the parts of it
that a header comment cannot: why it is shaped this way, and what it does *not*
promise. If you are writing your first zapp, start with
[writing-a-zapp.md](writing-a-zapp.md) and come back here for the rules.

**Current ABI version: 0.7.** That is not the project's version, which is
0.1.0 — the two count different things and move independently. The ABI number
is what a compiled `.llext` is checked against at load, so it advances whenever
the host gains a call, and it has been doing that since well before there was
anything to release. Every "added in 0.4" note below is therefore about *which
hosts have this*, not about which release you can download; a zapp declaring
`abi_minor` 4 runs on a 0.7 host, and one declaring 7 is refused by a 0.4 host.

| ABI | What arrived |
|---|---|
| 0.1 | Windows and labels |
| 0.2 | `set_user_data()` / `get_user_data()` — instances of one zapp share `.bss` |
| 0.3 | Storage: the permission shim, quotas, and a filesystem a zapp can reach |
| 0.4 | Resize, minimise, and `ZD_EV_WINDOW_CLOSE_REQUEST` |
| 0.5 | Keyboard input, text widgets, the clipboard, menu bars, dialogs, the clock |
| 0.6 | Lists, `dialog_prompt()`, picker navigation, `zapp_launch()`, singleton enforcement |
| 0.7 | Cell grids, timers, a ceiling on `window_set_geometry()`, `ZD_DLG_OK_ONLY` |

## Shape

A zapp is an ELF relocatable (`.llext`) that exports exactly one symbol,
`zd_zapp_manifest`, and imports (at most) one, `zd_get_host_api`.

```
desktop  --- struct zd_host_api *  ---> app   (vtable, passed to init)
app      --- struct zd_zapp_manifest ---> desktop (via LL_EXTENSION_SYMBOL)
```

Everything a zapp can do, it does through the vtable it is handed at `init()`.
A vtable rather than a pile of exported functions because it:

- keeps the export surface at one symbol,
- gives the ABI a version gate that can be checked before any zapp code runs,
- gives every call a natural place to hang the per-instance permission check,
- lets trusted and untrusted zapps be handed *different tables*, so an untrusted
  zapp has no privileged function to call rather than one that checks and
  refuses.

`zapp_abi.h` includes no Zephyr and no LVGL headers — only `<stdint.h>` and
`<stddef.h>`. That is what keeps LVGL's ABI from silently becoming ours.

## Versioning

- `abi_major` must match **exactly**.
- `zapp.abi_minor <= host.abi_minor` is accepted.
- The host vtable grows **by appending only**. `struct_size` lets an older zapp
  bind safely against a newer host.
- `enum zd_event_type` and the `ZD_KEY_*` constants grow by appending only too.
  They are wire format: a zapp compares against the numbers it was compiled
  with, and inserting a value renumbers everything after it with nothing in the
  versioning machinery to notice. The key codes are spelled as `#define`s with
  explicit values so this is impossible to miss.
- The **event union may grow**, and did in 0.5, 0.6 and 0.7. That is safe
  and `sizeof(struct zd_event)` is deliberately *not* pinned: a zapp only ever reads the event
  through a pointer the desktop handed it, and only members it knows about. What
  may not change is where anything already there lives, which the header states
  as `_Static_assert`s on every existing member's offset. Offsets are the rule;
  size is not.
- Anything else is a major bump.

Both checks run in `validate_manifest()` before `init()` is called. `zapps/badabi`
exists solely to exercise the rejection path; it declares `ZD_ABI_MAJOR + 1` and
is installed alongside the working zapps so the gate is exercised in the field
rather than in a test directory nobody runs. **If it ever launches, the gate is
broken.**

## Ordering guarantees

1. **No event reaches `event()` until `init()` has returned.** Creating a window
   focuses it, so without this a zapp would be told it has focus before it had
   recorded the handle it was just given — and could not tell its own window
   from another instance's. The desktop suppresses events during `init` and
   re-asserts focus afterwards.
2. **`fini()` runs before the extension is unloaded**, and after all of the
   zapp's windows are gone.
3. **Nothing is destroyed inside a callback.** Window close and instance unload
   are both queued and completed from the desktop loop, guarded by
   `wm->in_zapp_callback`. A zapp closing its own window is running on a stack
   frame inside text that unloading would free.

## Instances share one image

This is the least obvious property of the whole system, and it bites.

`llext_load()` refcounts extensions **by name**. Launching the same zapp twice
loads the ELF **once** and hands both instances the same code, the same `.data`
and the same `.bss`. A file-scope variable in a zapp is therefore shared across
every instance of that zapp.

```c
static zd_window_t window;   /* WRONG: the second instance stamps on the first */
```

Anything per-instance goes through `set_user_data()` / `get_user_data()` (added
in 0.2 for exactly this reason). Small values ride in the pointer itself; larger
state needs an allocation the zapp owns and frees in `fini()`.

The loader consequences are equally easy to get wrong, and are handled in
`zapp_instance.c`:

- `llext_load()` returns **negative** on failure, **0** on a fresh load, and a
  **positive** previous use-count when the image was already resident. Treating
  non-zero as an error makes the second instance of every zapp fail to start.
- `.init_array` runs once per image, not once per instance.
- `.fini_array` must run only when the *last* instance drops the image.

## Handles

A zapp never holds a pointer to a desktop object. It holds a handle packing a
slot index and a generation counter. Every call checks the slot is live, the
generation matches, the kind matches, and the object belongs to the caller.

Windows, labels, open files and open directories all go through the same table.
That files fitted with nothing but a new `enum zd_handle_kind` value is the first
evidence the scheme generalises beyond the WM it was written for.

Without an MMU this is the only part of the isolation story that genuinely
works. It does not stop a malicious extension. It turns the overwhelmingly
common failure — use-after-close — from a use-after-free into `-EINVAL`, and
stops one zapp touching another's windows by guessing. `zd_selftest_run()`
asserts all of this at boot, including that a stale handle does not resolve into
a slot's *new* occupant.

## Storage

Added in 0.3. Before it, `path_resolve()` could tell a zapp where its home
directory was and there was no call that could then do anything with the answer:
`fs_shim.c` described itself as the choke point every zapp filesystem call
passes through, and nothing passed through it.

Paths are absolute, and every call runs them through `zd_fs_resolve()` first:
`..`, relative paths, anything outside the session's roots, and any write to a
read-only root are refused with `-EINVAL` or `-EACCES`. Build paths from
`path_resolve()`; a zapp cannot know whether the root is `/RAM:` or `/SD:`, and
should not.

| flag | meaning |
|---|---|
| `ZD_O_READ` / `ZD_O_WRITE` | at least one is required |
| `ZD_O_CREATE` | create if absent |
| `ZD_O_APPEND` | seek to end after opening |
| `ZD_O_TRUNC` | discard existing contents |

### Reads and writes may be short

This is a contract, not an implementation detail. One call moves at most
`CONFIG_ZD_FS_IO_CHUNK` bytes (4096 by default) and may move fewer. Zapps loop.

The reason is the threading model. Zapps run as callbacks on the desktop thread,
so a transfer stops the entire UI while it runs — and on the CoreS3, where the
SD card's MISO line is also the display's data/command pin, the screen
*physically cannot be drawn* for the duration. Bounding one call bounds the
stall. Making I/O asynchronous instead would mean giving zapps their own thread,
which is `ZD_ZAPP_FLAG_WANTS_THREAD`, still reserved.

### Bounds

- `CONFIG_ZD_MAX_OPEN_FILES` (8) and `CONFIG_ZD_MAX_OPEN_DIRS` (4), desktop-wide,
  from fixed arrays rather than the heap — a leak announces itself as exhaustion.
- `CONFIG_ZD_MAX_OPEN_PER_ZAPP` (4) counts files and directories together, so one
  instance cannot starve the desktop.
- Everything an instance still holds is closed at teardown, after `fini()` — but
  the quota is small enough that a zapp relying on that will fail first.

### What it does not enforce

Everything in [What the desktop does not
enforce](#what-the-desktop-does-not-enforce) below still applies, unchanged.
The shim is the only *linkable* route to the filesystem, not the only possible
one: with no MMU a loaded extension is trusted code in the kernel address space.
Permissions here are advisory and filesystem-level. Saying otherwise would be
the one genuinely dishonest thing this document could do.

`zd_selftest_run()` asserts the refusals, the round trip, `-EBADF` on a closed
handle, the kind and owner checks, the quota and the teardown — at boot, on
every target, because the interesting failures are configuration-dependent.
`zd_selftest_run_wm()` adds more once the window manager exists. Both print
their results to the console at every boot, so the current count is whatever the
last line of `selftest (wm):` says rather than a number written down here.

## Window state

Since 0.4 a window can be resized by its grip and minimised by its titlebar
button or its taskbar entry, and the zapp is told about both.

| event | when |
|---|---|
| `ZD_EV_RESIZED` | the content area settled at a new size |
| `ZD_EV_MINIMIZED` / `ZD_EV_RESTORED` | the window was unmapped or mapped again |

**Minimising is not closing.** A minimised window keeps its handle, its widgets
and its place in the stacking order — it is unmapped, which means it draws
nowhere and cannot be clicked. Restoring puts it back where it was in the order
and raises it. `window_minimize()` and `window_restore()` let a zapp do to itself
what the user can do to it.

`ZD_EV_RESIZED` carries the **content area's** size, not the frame's. That is the
rectangle a zapp lays widgets out in, and the only one it should ever have to
reason about; the chrome's dimensions are the desktop's business and are not in
the ABI at all. Since 0.5 `window_get_content_size()` answers the same question
at any time, which is what a zapp needs at `init()` — before that there was no
way to know and the advice was to guess and refine on the first resize.

**`window_set_geometry()` has a ceiling as well as a floor**, as of 0.7. Below
the desktop's minimum window size a request is raised; above the usable area it
is lowered — a window taller than the screen has its resize grip under the
taskbar and, once moved, its titlebar off the top, so it can be neither resized
nor dragged back. The ABI has always said to check `window_get_geometry()`
rather than assume you got what you asked for; this is the same sentence with
the other inequality.

That ceiling is also **how a zapp finds out how much room there is**, and it is
why there is no screen-size call and should not be. What a zapp may have is not
the size of the panel — it is whatever the desktop is willing to give it, which
is a different and more useful number, and one that would change the day this
desktop grows a dock or a second monitor. Ask for an absurd window, read back
what arrived, lay out inside that, and resize to what you actually need.
Minesweeper picks its board this way, which is why it comes up 9×9 on a 480×272
panel and 9×3 on a 320×240 one without knowing either number.

**`ZD_EV_RESIZED` arrives on release, not during the drag.** A zapp callback may open files,
and on the CoreS3 the filesystem borrows the display's pin, so one filesystem-
capable callback per pointer sample would stop the screen for the length of a
drag. The content area tracks the pointer live; only the zapp's own widgets lag
until the button comes up.

### Closing is an ask you may decline, but not ignore

`ZD_EV_WINDOW_CLOSE_REQUEST` is delivered as of 0.4. Flush what you must and
call `window_close()`.

**0.5 sharpened the rule 0.4 stated.** There are three things a zapp can do with
the request, and they are not the same:

| what you do | what happens |
|---|---|
| `window_close()` | the window goes, which is what was asked |
| `window_close_cancel()` | the window stays; the user's next click asks again |
| nothing | the window goes anyway after `CONFIG_ZD_CLOSE_GRACE_MS` |

0.4 had only the first and the third, which made "no" and "wedged" the same
answer — and therefore made a Cancel button on a *save changes?* box pure
decoration, since the window went two seconds later regardless. Notepad is the
first zapp with something real to say here and it made that obvious.

The grace period is aimed at silence, not at refusal. A zapp that has stopped
answering — faulted, wedged, or never listening — loses its window and gets a
warning naming it in the log; a second click on the close box while a request is
still outstanding does the same immediately, which is the force-quit. A window
with no owner, or one whose zapp has no `event()` callback, closes at once
rather than stalling for a grace period nobody was going to use.

The desktop also **stops counting down while a dialog that zapp asked for is on
screen**. A zapp showing "save it?" is doing exactly what the close request
asked of it, and taking the window out from under the question would be absurd.

What this costs is honest: a zapp determined to decline forever can keep its
window. That is the bargain every desktop makes, and every desktop answers it
with an end-task of some kind. This one does not have one yet.

## Keyboard input

`ZD_EV_KEY` was a reserved enum value from 0.1 and is delivered as of 0.5.

Presses only. Releases are not delivered: nothing an application does needs
them, and they would double the traffic through a dispatch path that runs on the
desktop thread. Auto-repeat, when it arrives, will look like more presses.

Codes are the desktop's own `ZD_KEY_*`, not Linux input codes and not LVGL's
`LV_KEY_*` — for the same reason `ZD_O_READ` is not `FS_O_READ`. Anything that
produces a character arrives as `ZD_KEY_CHAR` with the codepoint in
`ev->key.unicode`; everything else is a named key. Enter, Tab and Backspace are
named rather than characters, because an editor treats them as commands far more
often than as text.

**Which keys reach you depends on what has the caret.** If your window has a
focused text widget, ordinary typing goes into it and never becomes an event. A
key held with CTRL always reaches you instead, so accelerators work while the
user is typing — this is the Win95 rule, and it is why `Ctrl+S` is not an `S`.
A window with no text widget gets everything.

There are two sources and you cannot tell them apart: a real keyboard, and the
desktop's on-screen keyboard. That is deliberate. The CoreS3 is a touch panel
with no keys at all, and it is the only target this project has run on silicon.

## Text widgets

`zd_text_t` is an editable multi-line field: caret, word wrap, scrolling,
click-to-position, drag-to-select. `label_create()` shows a string; this edits
one.

**Positions are byte offsets, everywhere** — the caret, the selection, the offset
into `text_get_text()`. Not character indices. Bytes are what a zapp has after
`fs_read()`, and converting is the desktop's job.

`text_get_text()` is **short by contract**, exactly like `fs_read()` and for the
same reason: a document outgrows any single call worth making on the desktop
thread. Loop until it returns 0. `text_get_capacity()` reports the largest
document the widget will hold — ask, do not assume, because the bound is the
desktop's and differs between boards.

`ZD_EV_TEXT_CHANGED` fires only for edits the **user** made. Changes you made
yourself, through `text_set_text()`, `text_insert()`, `text_paste()` and the
rest, do not come back to you: you already know about them, and being re-entered
from inside your own host call is a trap rather than a service. This is what
makes it usable as a dirty flag.

## The clipboard

One desktop-wide buffer of bytes, outliving the zapp that filled it. This is the
first thing in the ABI that is a property of the *desktop* rather than of your
instance, which is exactly what makes pasting into another application work.

No ownership negotiation, no formats, no lazy rendering. X11's selection
protocol is what taking those seriously looks like and none of it earns its keep
here. A copy larger than `CONFIG_ZD_CLIPBOARD_MAX` is truncated and logged, not
refused: half a paste is recoverable, a copy that silently produced nothing is
not.

`text_cut()`, `text_copy()` and `text_paste()` are provided rather than left to
each zapp, because the empty case of each is where a reimplementation goes wrong
— and because two applications should agree about what Ctrl+V does. A cut copies
first and only deletes if the copy succeeded.

## Menus

The desktop draws them; you say what is in them and are told which item was
chosen, as `ZD_EV_MENU` carrying an id you picked. A menu is chrome: it has to
match the palette, behave the same in every application, and be sized by touch
rules a zapp has no way to know.

The bar lives inside your window, above your content area, and **the content
area shrinks to make room**. Your coordinates do not move — they were always
relative to the content area, which is now shorter — and adding a bar delivers
`ZD_EV_RESIZED` so widgets laid out before it can be fixed up.

Only one drop-down is open at a time, desktop-wide, and `ZD_EV_MENU` is
delivered *after* it has been dismissed. Closing your own window from that
handler is therefore safe, which is what File → Exit needs.

## Lists

Added in 0.6, and the reason a file browser was not possible before it.
`label_create()` shows a string and `text_create()` edits one; neither is a way
to choose between many things, and a zapp cannot draw its own rows because it
never sees an `lv_obj_t`.

**A list is a model, not a picture**, and that is the whole of what a zapp needs
to understand. `list_clear()` and `list_add_item()` change what the list *is*,
immediately: `list_get_count()`, `list_get_selected()` and `list_get_item_text()`
all answer from the model the instant you call them. Only the rows on screen lag,
by at most one turn of the desktop loop.

That is what makes it safe to empty and refill a list from inside
`ZD_EV_LIST_ACTIVATE` — which is precisely what "the user opened this directory"
means, and which would otherwise be the use-after-free this whole project is
organised around avoiding. It is also not a new idea: it is the rule the window
manager already applies to stacking order, where the `sys_dlist_t` is the truth
and LVGL's child order is a projection re-applied from the loop.

Two events:

- `ZD_EV_LIST_SELECT` — the selection moved, by click or by arrow key.
  **Cheap by contract.** It fires on every arrow key, so do no filesystem I/O in
  it. Whatever you want to show about the selected thing you already had while
  you were filling the list — `fs_readdir` hands you the size and the type — so
  cache it there. On a board where storage borrows the display's pin, a stat per
  keystroke stops the screen drawing while the user holds Down. Same reasoning
  as `ZD_EV_RESIZED` arriving on release rather than per pointer sample.
- `ZD_EV_LIST_ACTIVATE` — a row was double-clicked, or Enter was pressed on it.

`list_clear()` clears the selection rather than preserving the index. Row 3 of
the new contents is not the thing row 3 used to be, and carrying it across is how
you delete the wrong file.

`list_get_capacity()` exists for the same reason `text_get_capacity()` does: the
bound is the desktop's, it differs between boards, and a zapp that guessed would
either refuse to show a directory it could have shown or stop listing partway
down one without saying so. `list_add_item()` past it returns `-ENOSPC`.

`list_get_item_text()` is **not** short by contract, unlike `text_get_text()` and
`fs_read()`. A row is one line, and half a filename still names a file — so it
refuses with `-ENOSPC` rather than truncating, the same promise
`dialog_get_path()` makes.

Double-click is LVGL's, not the desktop's. `lv_indev` already tracks click
streaks, and its thresholds are better than a hand-rolled timer's would have
been: the interval is the long-press time and the movement tolerance is the
scroll limit — *the same number that decides whether the gesture was a scroll*.
A finger that travelled far enough to scroll the list was scrolling it.

## Grids of cells

Added in 0.7, and the answer to a question the other three content types kept
raising and could not settle: **how does a zapp draw something the desktop has
no widget for?** A label shows a string, a text field edits one, a list chooses
between many. None of them can put a game board, a keypad, a colour palette or a
character map on screen, and a zapp cannot draw one itself, because it never
sees an `lv_obj_t`.

A grid is a rectangle of small square cells, each carrying a short string, a
text colour and one of three bevels — `ZD_CELL_RAISED`, `ZD_CELL_SUNKEN`,
`ZD_CELL_FLAT`. The desktop paints them, so they match the rest of the chrome
without a zapp knowing what a bevel is.

**The cells are a model and the picture is derived**, the same rule lists
follow. Here it is not a discipline but a fact about the implementation: a grid
is *one* LVGL object that draws every cell itself, so there are no child widgets
to create, destroy, or be standing on when a callback rewrites the board.
Rewriting all eighty-one cells from inside `ZD_EV_GRID_CLICK` is an ordinary
thing to do, and unlike every other widget here it needs no explanation of why
it is safe.

**Do not do the arithmetic yourself.** A cell's size is the desktop's, it scales
with the board's touch slop, and it is deliberately not reported as a number to
multiply — a zapp that laid out its own grid would be right on one target and
wrong on the next. Two calls, neither of which needs a grid to exist:

- `grid_measure(cols, rows, &w, &h)` — how much room a grid would take.
- `grid_fit(w, h, &cols, &rows)` — the largest grid that fits in a box. Either
  answer may be 0, which means there is no grid worth creating.

`grid_get_capacity()` bounds `cols * rows` and follows the same "ask, do not
assume" rule as `text_get_capacity()` and `list_get_capacity()`. Note that in
practice the pixels run out long before the capacity does, which is what
`grid_fit()` is for.

One event, `ZD_EV_GRID_CLICK`, with `ev->grid.action`:

- `ZD_GRID_PRIMARY` — a click, or a tap.
- `ZD_GRID_SECONDARY` — a press held down.

**A gesture produces exactly one of the two, never both.** There is no second
mouse button anywhere in this project — LVGL's pointer indev has one, the virtio
tablet reports one, and a fingertip has one — so a held press is how a cell gets
a second verb. The desktop swallows the click that a long press would otherwise
also produce on release, so a zapp never has to unpick the pair. Minesweeper is
the worked example: tap to uncover, hold to flag, and getting the ordering wrong
would flag a mine and immediately step on it.

Press feedback is deliberately absent: a cell is not drawn sunken while your
finger is on it. That needs an invalidate per pointer sample, and on a touch
panel your thumb is over the cell anyway.

## Timers

Also 0.7, and much smaller: `timer_start(period_ms, id)` and `timer_stop(id)`,
arriving as `ZD_EV_TIMER`.

This is **the first thing in the ABI that lets a zapp run when the user has done
nothing**. Everything up to 0.6 was strictly reactive — a zapp ran inside a
callback or it did not run — which was exactly right for a text editor and a
file browser and is not enough for a clock, a progress bar, or a game with a
stopwatch in the corner.

- A timer belongs to your **instance**, not to a window, and `ev->win` is
  therefore `NULL`. That makes it the first event the usual `ev->win != mine`
  guard cannot filter, so check the type first.
- Periods are **rounded up** to a floor the desktop sets. This is not a frame
  clock and there is no way to ask for one: zapps run as callbacks on the thread
  that draws the screen, so a zapp woken every millisecond is a desktop that
  never repaints.
- It is late, not exact. Use `uptime_ms()` to *measure* elapsed time and a timer
  to know when to look — a game that counted ticks would lose a second every
  time a cascade took longer than the period.
- Starting an id that is already running re-arms it rather than making a second.
- Your timers are stopped for you when your instance goes. That is not tidiness:
  a timer outliving the code it dispatches into is a use-after-free on a clock.

## Dialogs

Three, all provided by the desktop: a confirm box, a file picker that walks the
filesystem through the session's permissions, and a one-line text prompt.

**All three are asynchronous.** They return as soon as the dialog is up and the answer
arrives later as `ZD_EV_DIALOG`. There is no modal loop anywhere in this project
and there is not going to be — the desktop thread runs LVGL, so blocking it to
wait for a click would stop the thing being clicked from drawing.

A file dialog's answer is fetched with `dialog_get_path()` rather than carried in
the event: `ZD_PATH_MAX` does not belong in a union every event carries. It is
never truncated — a short path still names a file, just the wrong one.

Only one dialog exists at a time desktop-wide; asking while one is up returns
`-EBUSY`. It is **system modal** rather than application modal, which is a
simplification and the opposite of what Win95 did. Clicking outside it does
nothing, also deliberately: a *save changes?* that answers itself because the
user tapped the wrong place is how work gets lost.

A dialog belongs to the instance that asked for it. If that zapp dies with one
open, the desktop takes it down — otherwise a modal shade outlives its owner and
the whole screen stops responding.

`ZD_DLG_OK_ONLY` arrived in 0.7, for **telling rather than asking**. "This
folder is read-only" and "here is how to play" are not questions, and the Cancel
button beside them was answering one nobody had asked. Three zapps were already
showing messages through `ZD_DLG_OK_CANCEL` before the constant existed, which
is what made it worth adding rather than a matter of taste. Escape and the close
box still report `ZD_DLG_CANCEL` — a box you can only dismiss by finding the
right button is worse than a spurious button — so a zapp showing one of these
ignores the result entirely.

`dialog_prompt()` arrived in 0.6 with the file browser. Confirm answers a
question the desktop asked and the picker answers "which of these"; neither
answers "what shall it be called", which is what naming a new folder needs. Its
answer comes back from `dialog_get_text()`, on the same split — the event says
what happened, a call says how much. An empty field is not an answer: OK stays
inert until something is typed, and Cancel is how the user declines.

The picker gained **directory navigation** in 0.6. It shows folders as well as
files, enters them on a double-click, and comes back up through a `..` row that
is floored at the directory you named — a picker opened on `ZD_DIR_TMP` cannot
walk out of `/tmp`. There is deliberately no `enum zd_dir` member naming the
parent of anything: the floor is a string the caller already gave, and asking the
session for one would make "up" mean something different depending on where you
started.

## Launching another zapp

`zapp_launch(name, arg)`, added in 0.6, is the second thing in this ABI that is
a property of the desktop rather than of your instance — the clipboard was the
first — and it is what makes double-clicking a document mean anything.

`name` is a **discovered zapp name, not a path**. It resolves through the same
scan the Start menu uses, so this cannot be talked into `llext_load()`ing an
arbitrary file. Given how carefully the rest of this document hedges about
permissions, it is worth saying that this one is a real property of the design.

`arg` is yours to define with whoever you are launching. **There is no
association registry.** A zapp that opens documents decides what it accepts, and
a zapp that launches one has to know; the file browser hardcodes `notepad` and
says so in its own source rather than implying a lookup that does not exist.

Asynchronous, like the dialogs and for a sharper version of the same reason:
loading an extension reads a file and runs its `init()`, neither of which may
happen on a stack frame inside your event callback. Everything checkable comes
back from the call — a bad name, an unknown zapp, no free slot, an argument too
long. Only what cannot be known without doing the work is logged instead: a
corrupt image, a refused ABI, llext out of heap.

`ZD_ZAPP_FLAG_SINGLETON` is **enforced as of 0.6**, having been defined and
ignored since 0.1. A running singleton has its frontmost window raised and
focused and is sent `ZD_EV_LAUNCH_ARG` instead of getting a second instance.
Read what was asked for with `get_launch_arg()`, which is valid from `init()`
onwards for the life of the instance.

## The clock

`clock_now()` reports the same time the taskbar shows, deliberately: two
independent guesses disagreeing on one screen is worse than one guess. And on
the targets so far it *is* a guess — `qemu_cortex_a53` has no RTC node and
Zephyr has no PL031 driver, so the desktop counts up from a fixed start rather
than showing 00:00 since boot. A board with an RTC makes every caller correct at
once without any of them changing.

`uptime_ms()` remains the right call for measuring an interval. `clock_now()` is
for showing a person a time.

## The exported symbol surface

`CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n`. Measured on this tree:

| configuration | symbols exported to extensions |
|---|---|
| Zephyr defaults (`UNASSIGNED`, `SYSCALL`, `LIBC`) | 172 |
| this project | **1** (`zd_get_host_api`) |

Narrowing it cost nothing, because zapps import nothing else — they reach
everything through the vtable. Without this, `fs_open()` and the whole syscall
surface are linkable from an extension, and the permission shim can be bypassed
by simply not calling it.

**This does not make the shim a security boundary.** Without an MMU a loaded
extension is trusted code in the kernel address space and can reach any address
it can compute. What the narrow surface buys is that the shim is the only
*linkable* route, which is the difference between a contract and a suggestion.
Real enforcement needs `CONFIG_USERSPACE` plus `llext_add_domain()`, which is
reachable on the two ARM targets (`ARCH_HAS_USERSPACE if ARM_MMU` on arm64, `if
ARM_MPU` on the RT1060's Cortex-M7) and deferred there rather than designed out.
On the ESP32-S3 it has not been investigated, so treat it as unanswered rather
than as either promised or ruled out. Every host-API entry point
already carries a `zd_zapp_ctx_t`, so those calls become syscalls without a zapp
changing a line.

## What the desktop does not enforce

Stated plainly so nobody mistakes the shim for more than it is:

- **Memory safety.** No MMU/MPU isolation. A wild pointer takes the system down.
- **Blocking.** A zapp that loops forever in a callback hangs the desktop. A
  watchdog that can kill a zapp needs the zapp to own a thread —
  `ZD_ZAPP_FLAG_WANTS_THREAD` is defined and reserved, not honoured.
- **Closing forever.** A zapp *can* decline a close with
  `window_close_cancel()` — see [Closing is an ask you may decline, but not
  ignore](#closing-is-an-ask-you-may-decline-but-not-ignore). What it cannot do
  is ignore one: a zapp that neither answers nor cancels gets closed anyway when
  the grace period expires. A zapp that *does* answer, with a dialog up, has the
  grace period extended for as long as the dialog stands — so clicking the close
  box a second time is the only force-quit there is. A real end-task, in the
  taskbar where every other desktop puts one, is deferred.
- **Storage quotas in bytes.** A zapp may fill the volume.

## Build

Zapps live in `zapps/` — not `apps/`, to keep them distinct from Zephyr's
convention where `app/` is the application source directory, which this project
also has. They are built by the desktop's CMake but are separate ELF artifacts:
one `west build` produces `zephyr.elf` and a `.llext` for every name in
`ZD_ZAPP_NAMES`.

**A zapp may be several source files.** ARM and ARM64 *default* to
`LLEXT_TYPE_ELF_OBJECT`, which is one compiler invocation and therefore exactly
one file, so both ARM board fragments select `LLEXT_TYPE_ELF_RELOCATABLE`
instead; Xtensa defaults to `LLEXT_TYPE_ELF_SHAREDLIB`, which never had the
restriction. `notepad` is four files and `files` is four. The one exception is
deliberate: `hello` stays a single translation unit, so that the simplest
possible zapp is still the simplest possible zapp.

See [writing-a-zapp.md](writing-a-zapp.md) for the registration steps and the
traps — chiefly that a zapp has no libc.

> **Build correctness note.** On Zephyr `main` @ `e201b84b`, `add_llext_target`'s
> packaging step does not re-run when a source changes: the `_debug.elf` command
> depends on a phony target with no file-level dependency on the object, so the
> `.obj` recompiles and the `.llext` silently stays stale. A zapp edit then ships
> the *previous* binary with no warning. `app/CMakeLists.txt` re-attaches the
> missing dependency to the documented `pkg_input` property. Without that fix,
> only `west build -p` produces a correct zapp binary.
