# zephyr-desktop

A retro desktop shell on [Zephyr RTOS](https://zephyrproject.org) + [LVGL](https://lvgl.io):
overlapping draggable windows with hand-built Win95-era chrome, a taskbar with a
launcher and clock, and **zapps as `.llext` extensions discovered on a filesystem
and loaded at runtime**.

It runs on `qemu_cortex_a53` — natively on macOS, in a real window, with a real
mouse — and builds for the MIMXRT1060-EVK and the M5Stack CoreS3.

```
+--------------------------------------------------+
| +--------------------------+                     |
| | Hello (active)       [X] |                     |
| +--------------------------+  <- drawn by a 3 KB |
| | hello world, Zephyr!     |     .llext loaded   |
| |                          |     at runtime      |
| +--------------------------+                     |
|                                                  |
| [ Start ]                               [ 9:41 ] |
+--------------------------------------------------+
```

## Status

The MVP is complete. Boot to a retro desktop; the launcher enumerates zapps found
on the filesystem; clicking one loads its `.llext` at runtime; the zapp calls into
the desktop API to create a window and draw "hello world, Zephyr!"; two instances
can be open, dragged over each other with correct z-order and focus; closing one
unloads the extension cleanly with no leaks.

Verified over 20 consecutive launch/close cycles: 20 loads, 20 unloads, zero
errors, and after every one — 0 windows live, 8 slab blocks free, 0 zapps live,
0 handles live.

Since then the ABI has grown one zapp at a time, and only ever by appending.
It now ships a **text editor** (typing, selection, clipboard, menus, dialogs),
a **file browser** that navigates the filesystem and hands a `.txt` to the
editor, and **Minesweeper** — which was the useful one, because it is the only
zapp so far that is not an application. It asked for a way to draw something the
desktop will never have a widget for, and for the ability to do anything at all
without being clicked. Both had been missing and neither had been noticed.

It also runs **on hardware**: the whole criterion above, by touch, on an
**M5Stack CoreS3** — 320×240 touchscreen, Xtensa rather than ARM, zapps loaded
off a microSD card. Getting the card working meant arbitrating GPIO35, which
this board wires to both SPI MISO and the LCD's D/C line; upstream's own answer
is to disable the display. The `mimxrt1060_evk` builds (324 KB, headless) but
has not been run on silicon. See [docs/hardware.md](docs/hardware.md).

## Quick start

Requires the Zephyr SDK and `west`. On macOS, QEMU's `cocoa` display backend
does the rest — no VM, no X server.

```sh
git clone https://github.com/beriberikix/zephyr-desktop
cd zephyr-desktop
west init -l manifest
west update
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1
west build -b qemu_cortex_a53 app
west build -t run          # opens a window; click Start
```

To verify a change without a human looking at a window, `tools/shot.py` drives
QEMU over QMP — injecting pointer events and screenshotting the framebuffer:

```sh
python3 tools/shot.py -d build -o /tmp/out \
    --shot boot --click 0.067,0.949 --shot menu
```

It reports pixels changed between shots, which is usually the assertion you
actually want. `tools/zoom.py` magnifies a region, because two one-pixel bevel
rings cannot be judged at 1:1.

## Writing a zapp

A zapp is one C file that includes exactly one header and links against
essentially nothing:

```c
#include <zephyr/llext/symbol.h>
#include <zd/zapp_abi.h>

static int hello_init(zd_zapp_ctx_t ctx, const struct zd_host_api *api)
{
        struct zd_window_desc desc = { .title = "Hello" };
        zd_window_t win = api->window_create(ctx, &desc);

        api->label_create(ctx, win, "hello world, Zephyr!", 8, 8);
        api->set_user_data(ctx, (void *)win);
        return 0;
}

struct zd_zapp_manifest zd_zapp_manifest = {
        .magic = ZD_ZAPP_MAGIC,
        .abi_major = ZD_ABI_MAJOR,
        .abi_minor = ZD_ABI_MINOR,
        .name = "Hello",
        .init = hello_init,
};
LL_EXTENSION_SYMBOL(zd_zapp_manifest);
```

No LVGL, no Zephyr, no idea where its window comes from or what draws it. Drop
it in `zapps/<name>/<name>.c`, add the name to `ZD_ZAPP_NAMES` in
`app/CMakeLists.txt`, and it builds as a separate `.llext` artifact.

(`zapps/`, not `apps/`: Zephyr's convention is that `app/` holds the application
source, and this project has one. A sibling `apps/` reads as a typo for it every
single time.)

Read [docs/abi.md](docs/abi.md) before writing a second one — particularly the
part about instances of one zapp sharing `.bss`.

## How it works

```
manifest/west.yml   the Zephyr pin (a main commit, deliberately — see below)
include/zd/         the zapp ABI. No Zephyr and no LVGL headers may appear here.
app/                the desktop image
  src/wm/           client struct, stacking, focus, drag, handle registry
  src/chrome/       retro bevels, titlebar, palette
  src/shell/        background, taskbar, launcher, clock
  src/host/         host-API vtable, fs shim, session, storage
  src/loader/       llext discover/load/instance/unload, boot seeding
zapps/              hello, notes, notepad, files, mines (not apps/ -- see below)
tools/              headless screenshot and zoom harness
```

Three ideas carry most of the weight:

**The WM is a tiny X11 stacking WM.** A `zd_client` per window, one
`sys_dlist_t` for z-order that is *authoritative* — LVGL's child order is a
projection re-applied from it — and one central dispatch path where every frame
carries a single callback and every child bubbles to it.

**Nothing is destroyed during dispatch.** A zapp closing its own window is
running on a stack frame inside text that unloading would free. Window close and
instance unload are queued and completed from the desktop loop, guarded by a
callback-depth counter.

**The ABI is a vtable, not a symbol pile.** The desktop exports one symbol; zapps
get opaque, generation-counted handles validated against their owner. That keeps
the symbol surface at **1** (Zephyr's defaults would export 172), so the
permission shim is the only linkable route to the filesystem.

## Honest limits

- **Permissions are advisory.** Without an MMU a loaded `.llext` is trusted code
  in the kernel address space. The fs shim is a contract, not a security
  boundary. `CONFIG_USERSPACE` + `llext_add_domain()` is the hardening path and
  is reachable on both targets — deferred, not designed out.
- **Zapps are callback-driven**, single-threaded. `ZD_ZAPP_FLAG_WANTS_THREAD` is
  reserved, not honoured.
- **No window resize**, no clipboard, no notifications, no sound, no theming
  engine, no real multi-user login, no hardware acceleration yet.
- **Zephyr is pinned to a `main` commit, not a release.** The `qemu_cortex_a53`
  display and pointer stack landed 2026-06-15, after the v4.4.1 tag. No release
  contains it.
- **Xtensa cannot stream zapps off the filesystem.** It requires writable llext
  storage, which requires a `peek()`-capable loader, and `llext_fs_loader` has
  none — so the CoreS3 reads each zapp into RAM first. Discovery is unchanged.
- **`native_sim` is not a target and cannot be.** `arch/posix/` has no `elf.c`
  and `arch_elf_relocate*` are weak stubs returning `-ENOTSUP`, so it builds
  happily with `CONFIG_LLEXT=y` and then fails every `llext_load()` at runtime.

## Documentation

| | |
|---|---|
| [docs/design.md](docs/design.md) | The design doc and milestone log, including everything the build taught us that the plan got wrong |
| [docs/abi.md](docs/abi.md) | The zapp ABI: versioning, ordering guarantees, handles, the symbol surface |
| [docs/hardware.md](docs/hardware.md) | Hardware runbooks: MIMXRT1060-EVK and M5Stack CoreS3 |
| [CLAUDE.md](CLAUDE.md) | Orientation and the rules that are easy to get wrong |

## Licence

Apache-2.0, matching Zephyr.
