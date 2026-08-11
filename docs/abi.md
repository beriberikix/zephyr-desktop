# The zephyr-desktop app ABI

The contract is `include/zd/app_abi.h`. This document explains the parts of it
that a header comment cannot: why it is shaped this way, and what it does *not*
promise.

Current version: **0.2**.

## Shape

An app is an ELF relocatable (`.llext`) that exports exactly one symbol,
`zd_app_manifest`, and imports (at most) one, `zd_get_host_api`.

```
desktop  --- struct zd_host_api *  ---> app   (vtable, passed to init)
app      --- struct zd_app_manifest ---> desktop (via LL_EXTENSION_SYMBOL)
```

Everything an app can do, it does through the vtable it is handed at `init()`.
A vtable rather than a pile of exported functions because it:

- keeps the export surface at one symbol,
- gives the ABI a version gate that can be checked before any app code runs,
- gives every call a natural place to hang the per-instance permission check,
- lets trusted and untrusted apps be handed *different tables*, so an untrusted
  app has no privileged function to call rather than one that checks and
  refuses.

`app_abi.h` includes no Zephyr and no LVGL headers — only `<stdint.h>` and
`<stddef.h>`. That is what keeps LVGL's ABI from silently becoming ours.

## Versioning

- `abi_major` must match **exactly**.
- `app.abi_minor <= host.abi_minor` is accepted.
- The host vtable grows **by appending only**. `struct_size` lets an older app
  bind safely against a newer host.
- Anything else is a major bump.

Both checks run in `validate_manifest()` before `init()` is called. `zapps/badabi`
exists solely to exercise the rejection path; it declares `ZD_ABI_MAJOR + 1` and
is installed alongside the working apps so the gate is exercised in the field
rather than in a test directory nobody runs. **If it ever launches, the gate is
broken.**

## Ordering guarantees

1. **No event reaches `event()` until `init()` has returned.** Creating a window
   focuses it, so without this an app would be told it has focus before it had
   recorded the handle it was just given — and could not tell its own window
   from another instance's. The desktop suppresses events during `init` and
   re-asserts focus afterwards.
2. **`fini()` runs before the extension is unloaded**, and after all of the
   app's windows are gone.
3. **Nothing is destroyed inside a callback.** Window close and instance unload
   are both queued and completed from the desktop loop, guarded by
   `wm->in_app_callback`. An app closing its own window is running on a stack
   frame inside text that unloading would free.

## Instances share one image

This is the least obvious property of the whole system, and it bites.

`llext_load()` refcounts extensions **by name**. Launching the same app twice
loads the ELF **once** and hands both instances the same code, the same `.data`
and the same `.bss`. A file-scope variable in an app is therefore shared across
every instance of that app.

```c
static zd_window_t window;   /* WRONG: the second instance stamps on the first */
```

Anything per-instance goes through `set_user_data()` / `get_user_data()` (added
in 0.2 for exactly this reason). Small values ride in the pointer itself; larger
state needs an allocation the app owns and frees in `fini()`.

The loader consequences are equally easy to get wrong, and are handled in
`app_instance.c`:

- `llext_load()` returns **negative** on failure, **0** on a fresh load, and a
  **positive** previous use-count when the image was already resident. Treating
  non-zero as an error makes the second instance of every app fail to start.
- `.init_array` runs once per image, not once per instance.
- `.fini_array` must run only when the *last* instance drops the image.

## Handles

An app never holds a pointer to a desktop object. It holds a handle packing a
slot index and a generation counter. Every call checks the slot is live, the
generation matches, the kind matches, and the object belongs to the caller.

Without an MMU this is the only part of the isolation story that genuinely
works. It does not stop a malicious extension. It turns the overwhelmingly
common failure — use-after-close — from a use-after-free into `-EINVAL`, and
stops one app touching another's windows by guessing. `zd_selftest_run()`
asserts all of this at boot, including that a stale handle does not resolve into
a slot's *new* occupant.

## The exported symbol surface

`CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n`. Measured on this tree:

| configuration | symbols exported to extensions |
|---|---|
| Zephyr defaults (`UNASSIGNED`, `SYSCALL`, `LIBC`) | 172 |
| this project | **1** (`zd_get_host_api`) |

Narrowing it cost nothing, because apps import nothing else — they reach
everything through the vtable. Without this, `fs_open()` and the whole syscall
surface are linkable from an extension, and the permission shim can be bypassed
by simply not calling it.

**This does not make the shim a security boundary.** Without an MMU a loaded
extension is trusted code in the kernel address space and can reach any address
it can compute. What the narrow surface buys is that the shim is the only
*linkable* route, which is the difference between a contract and a suggestion.
Real enforcement needs `CONFIG_USERSPACE` plus `llext_add_domain()` — reachable
on both targets (`ARCH_HAS_USERSPACE if ARM_MMU` on arm64, `if ARM_MPU` on the
RT1060's Cortex-M7) and deferred, not designed out. Every host-API entry point
already carries a `zd_app_ctx_t`, so those calls become syscalls without an app
changing a line.

## What the MVP does not enforce

Stated plainly so nobody mistakes the shim for more than it is:

- **Memory safety.** No MMU/MPU isolation. A wild pointer takes the system down.
- **Blocking.** An app that loops forever in a callback hangs the desktop. A
  watchdog that can kill an app needs the app to own a thread —
  `ZD_APP_FLAG_WANTS_THREAD` is defined and reserved, not honoured.
- **Refusing a close.** `ZD_EV_WINDOW_CLOSE_REQUEST` is defined but never sent;
  the WM closes unconditionally.

## Build

Apps live in `zapps/` — not `apps/`, to keep them distinct from Zephyr's
convention where `app/` is the application source directory, which this project
also has. They are built by the desktop's CMake but are separate ELF artifacts. One
`west build` produces `zephyr.elf` and `hello.llext`.

ARM/ARM64 default to `LLEXT_TYPE_ELF_OBJECT`, which permits exactly **one source
file per extension**. Lifting that means switching to
`LLEXT_TYPE_ELF_RELOCATABLE`.

> **Build correctness note.** On Zephyr `main` @ `e201b84b`, `add_llext_target`'s
> packaging step does not re-run when a source changes: the `_debug.elf` command
> depends on a phony target with no file-level dependency on the object, so the
> `.obj` recompiles and the `.llext` silently stays stale. An app edit then ships
> the *previous* binary with no warning. `app/CMakeLists.txt` re-attaches the
> missing dependency to the documented `pkg_input` property. Without that fix,
> only `west build -p` produces a correct app binary.
