# zephyr-desktop MVP — design doc + task list

> **Status: milestones A-E complete.** Workspace pinned, target strategy proven end to end,
> retro theme, taskbar, and a full stacking WM with drag, focus and deferred destruction
> running on `qemu_cortex_a53`, plus a FAT filesystem, session, path-scoping shim and a
> launcher that lists apps found on disk. Milestones F–H remain; see §7. Two findings from building A are folded in below, marked **[A]**.

## Context

Build the *spine* of a retro desktop shell on Zephyr + LVGL: overlapping draggable
windows with hand-built Win95/System-7 chrome, a taskbar with launcher and clock, and
apps that are real `.llext` binaries discovered on a filesystem and loaded at runtime.
The MVP is a scaffolding proof, not a pixel proof — success is "boot to desktop, launcher
finds apps on disk, click loads a `.llext`, app calls the desktop ABI to open a window
saying hello, two instances drag over each other with correct z-order and focus, closing
unloads the extension with no leaks."

Two pieces are load-bearing and get real design attention: the **WM data model + event
loop** (modelled on a tiny X11 stacking WM) and the **app ABI** (designed as if the
terminal, text editor, and file browser already ran on it; implemented only as far as
hello world needs). Everything else may be scrappy.

The repo at `/Users/jberi/code/zephyr-things/zephyr-desktop` is currently empty. This
plan starts from nothing.

---

## 1. Recon findings (verified against the tree, not from memory)

Verified against `/Users/jberi/code/zephyr-things/really-native-sim/zephyr`
(Zephyr `main`, VERSION 4.4.99, HEAD `7a8aaac0`, `main` at `e201b84b` 2026-07-31) and by
running your SDK's QEMU on this machine.

### 1.1 llext arch support — your suspicion was correct

- Every llext-capable arch has an `arch/<arch>/core/elf.c`. Present: `arm`, `arm64`,
  `riscv`, `arc`, `x86`, `xtensa`, `openrisc`. **`arch/posix/` has none.**
- `arch_elf_relocate`, `arch_elf_relocate_local`, `arch_elf_relocate_global` are `__weak`
  stubs returning `-ENOTSUP` (`subsys/llext/llext_link.c:29-46`). So `native_sim` with
  `CONFIG_LLEXT=y` **compiles and links, then fails every `llext_load()` at relocation.**
  No build error to warn you — a silent runtime dead end.
- `tests/subsys/llext/tests.yaml` `arch_allow` lists are exactly `{arm, arm64, riscv,
  arc, x86, xtensa, openrisc}`. Never posix. `doc/services/llext/index.rst` says the same
  (its list is slightly stale — omits openrisc).
- The docs' "x86" is `ARCH=x86` (qemu_x86), confirmed by `arch/x86/core/elf.c`. Not POSIX.

**Conclusion: native_sim can never load a `.llext`.** Your read holds.

### 1.2 The finding that changes the target strategy

`qemu_cortex_a53` is a **graphical, pointer-driven, llext-capable target that runs
natively on macOS**:

| Capability | Evidence |
|---|---|
| Display | `boards/qemu/cortex_a53/qemu_cortex_a53.dts:26` — `zephyr,display = &ramfb0`; driver `drivers/display/display_qemu_ramfb.c` |
| Pointer input | same dts `:27` — `zephyr,touch = &virtio_input0`; driver `drivers/input/input_virtio.c` |
| QEMU wiring, automatic | `boards/qemu/cortex_a53/board.cmake` adds `-device virtio-tablet-device,bus=virtio-mmio-bus.3` when `CONFIG_INPUT_VIRTIO` |
| **macOS window** | `cmake/emu/qemu.cmake:106` — `if(CMAKE_HOST_APPLE) set(QEMU_DISPLAY_BACKEND cocoa)`, then `-device ramfb -vga none -display cocoa,show-cursor=on` |
| llext, CI-tested | arm64 is an `arch_allow`; `qemu_cortex_a53` is the named `integration_platform` for `llext.readonly_mmu` |
| Userspace path exists | `arch/arm64/core/Kconfig:232` — `select ARCH_HAS_USERSPACE if ARM_MMU` |

Verified on your machine: `~/zephyr-sdk-1.0.1/hosttools/usr/bin/qemu-system-aarch64`
(QEMU 10.0.2) reports display backends `none, cocoa, dbus`, and devices `ramfb` and
`virtio-tablet-device`. Everything needed is present.

Also relevant: `samples/modules/lvgl/demos/boards/qemu_cortex_a53.overlay` sets ramfb to
**480×272** with the comment "keep software rendering responsive under TCG" — the exact
resolution of the RT1060's `rk043fn66hs_ctg` shield.

### 1.3 Version pin — must track `main`

`ramfb` is in v4.4.0, but `input_virtio.c` and the `qemu_cortex_a53` display/pointer
wiring landed **2026-06-15**, after the v4.4.1 tag (2026-06-10). They are in **no release
tag**. The project pins Zephyr `main` at a commit ≥ 2026-06-15; `e201b84b04e4` is verified
locally. LVGL is manifest rev `bbedf265` = **9.6.0-dev**; `lv_win` still exists
(`src/widgets/win/lv_win.c`).

### 1.4 llext API surface (current, verified)

- Loaders: `struct llext_fs_loader` + `LLEXT_FS_LOADER(path)` macro
  (`include/zephyr/llext/fs_loader.h`) — reads the ELF straight from a Zephyr filesystem.
  Storage type `LLEXT_STORAGE_TEMPORARY`, so llext copies into its heap.
- Lifecycle: `llext_load()` → `llext_bringup()` (runs `.preinit_array`/`.init_array`) →
  `llext_find_sym(&ext->exp_tab, name)` → call → `llext_teardown()` (`.fini_array`) →
  `llext_unload(&ext)`. `llext_bootstrap()` wraps bringup+fn+teardown with a
  `k_thread_create`-compatible signature for the future thread-per-app case.
- Symbol export, desktop → app: `EXPORT_SYMBOL(x)`, `EXPORT_SYMBOL_NAMED`,
  `EXPORT_GROUP_SYMBOL(GROUP, x)` (`include/zephyr/llext/symbol.h`). Groups are gated by
  `CONFIG_LLEXT_EXPORT_SYMBOL_GROUP_<GROUP>`.
- Symbol export, app → desktop: `LL_EXTENSION_SYMBOL(x)`, looked up in `ext->exp_tab`.
- **`CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=y` by default**, which turns on `UNASSIGNED`,
  `SYSCALL` and `LIBC` groups. This matters for the permission model (§5).
- Build tooling: `add_llext_target(<name> OUTPUT <file.llext> SOURCES <src>)` plus
  `llext_include_directories()` / `llext_compile_options()`
  (`cmake/modules/extensions.cmake:6109`). Runs inside the Zephyr app build, emits a
  separate `.llext` artifact. ARM/ARM64 default to `LLEXT_TYPE_ELF_OBJECT`, which allows
  **exactly one source file per extension**. True out-of-tree app builds later via
  `west build -t llext-edk` (`cmake/llext-edk.cmake`).
- `llext_add_domain(ext, &domain)` exists for the future `CONFIG_USERSPACE` hardening.

### 1.5 LVGL / Zephyr glue

- `LV_USE_OS = LV_OS_CUSTOM` with `lvgl_zephyr_osal.h`
  (`modules/lvgl/include/lv_conf.h:44`). `lvgl_lock()` / `lvgl_trylock()` /
  `lvgl_unlock()` in `modules/lvgl/include/lvgl_zephyr.h` under `CONFIG_LV_Z_LVGL_MUTEX`.
  This is the only sanctioned way to touch LVGL off the render thread.
- Pointer binding: `CONFIG_LV_Z_POINTER_FROM_CHOSEN_TOUCH` (default `y` when
  `zephyr,touch` is chosen) creates the `lv_indev` from the Zephyr input subsystem —
  a53's `virtio_input0` flows in with no code.
- `lv_timer_handler()` is called by the app's loop (`modules/lvgl/lvgl.c:244` shows the
  pattern). `CONFIG_LV_Z_FLUSH_THREAD` moves flushes off it.
- `modules/lvgl/lvgl_fs.c` bridges LVGL's FS layer to Zephyr's — free image/font loading
  later.

### 1.6 Filesystem options

- QEMU a53 has no disk. Options: `zephyr,ram-disk` (`dts/bindings/disk/zephyr,ram-disk.yaml`,
  props `disk-name` / `sector-size` / `sector-count`) with FAT or littlefs on top.
  There is **no virtio-blk disk driver**; `virtiofs` exists but is virtio-**pci** and
  x86-only, and needs `virtiofsd` (Linux-only) — dead end on macOS.
- Host→guest file channel that *does* work: **QEMU fw_cfg**.
  `qemu_fwcfg_find_file(dev, "opt/...", &sel, &size)` + `qemu_fwcfg_read_item()`
  (`include/zephyr/drivers/firmware/qemu_fwcfg/qemu_fwcfg.h`), fed by
  `-fw_cfg name=opt/...,file=host/path`. Reserved for later (§9, milestone G).
- Build-time embedding: `generate_inc_file_for_target()`
  (`cmake/modules/extensions.cmake:726`) turns a `.llext` into a C array.
- RT1060 later: `zephyr,sdmmc-disk` on `usdhc1` (`mimxrt1060_evk.dtsi:265`) → FAT on SD.

### 1.7 i.MX RT1060 as the hardware bet — yes

- `boards/nxp/mimxrt1060_evk` with `zephyr_lcdif` (`display_mcux_elcdif.c`) pinned out.
- Shield `rk043fn66hs_ctg` in-tree: 480×272, 16-bit parallel, Goodix GT911 touch. Also
  `rk043fn02h_ct`. You don't have a panel yet — see §8 milestone G.
- SD card for real `.llext` files (above).
- **2D accel is real and LVGL-visible**: `CONFIG_LV_USE_PXP` / `CONFIG_LV_USE_GPU_NXP_PXP`
  / `CONFIG_LV_Z_PXP_INTERRUPT_PRIORITY` exist in `modules/lvgl/Kconfig:216-228`. The PXP
  plugs in as an LVGL draw unit — nothing in the WM has to change, which is exactly the
  property you asked to preserve.
- Cortex-M7 = `arch: arm` → llext supported, `LLEXT_TYPE_ELF_OBJECT`, and
  `ARCH_HAS_USERSPACE if ARM_MPU` for the future hardening milestone.

Verdict: good bet. Same LVGL, same ABI, same WM; only the board overlay, the FS mount
table, and one Kconfig for PXP differ.

---

## 2. Target strategy

**Single MVP target: `qemu_cortex_a53`.**

One target does both jobs your two-target split was designed to cover — graphical UX
iteration *and* genuine runtime `.llext` loading — because it has ramfb + virtio-tablet +
arm64 llext, and Zephyr's own QEMU runner opens a cocoa window on macOS. So there is no
second link mode, no static-linked-app fallback, no Linux VM, and no risk of the app
model forking. That is a strict simplification of the original plan, not a compromise.

Consequences worth naming:

- `native_sim` is **dropped from the MVP entirely**, not deferred-with-a-hook. It cannot
  load extensions, so having it would immediately force the static-link fork you wanted
  to avoid. If it comes back later purely as a fast pixel-iteration target, it comes back
  behind the same ABI header with a `zd_app_register_static()` shim — but nothing in the
  MVP is shaped around that possibility beyond keeping `include/zd/app_abi.h` free of
  Zephyr and LVGL types.
- The a53 defconfig sets `CONFIG_QEMU_ICOUNT=y` (deterministic clock, added "to avoid
  timing skew in tests"). Expect to turn it off in the board conf for interactive feel;
  verify early.
- Software rendering under TCG at 480×272 is the reference performance point. Don't
  chase frame rate; it is not what the MVP proves.

Hardware target (`mimxrt1060_evk`) enters at milestone G as a **headless** checkpoint
since you have no panel: dummy display (`CONFIG_DUMMY_DISPLAY`, `zephyr,dummy-dc`), real
`.llext` off SD, real WM event loop, no pixels. Buying an `rk043fn66hs_ctg` later
upgrades that milestone to full graphical with an overlay change.

---

## 3. Architecture — WM

### 3.1 Layers

Three sibling LVGL containers on the active screen, created once at boot, whose order is
never permuted:

```
screen
├── layer_desktop   patterned background, full-screen, click = defocus-all
├── layer_windows   every client frame lives here; z-order == child order
└── layer_panel     taskbar/menu bar; permanently topmost
```

Raising a window is `lv_obj_move_foreground(frame)` **within `layer_windows`**, so the
panel can never be occluded and the background can never be raised. This removes an
entire class of stacking bugs for free.

### 3.2 The client struct

`app/src/wm/wm.h`:

```c
struct zd_client {
    sys_dnode_t             node;        /* in wm->stack; head == topmost */
    uint32_t                id;

    /* LVGL handles — the WM owns these; apps never see them (see §4.3) */
    lv_obj_t               *frame;       /* child of layer_windows */
    lv_obj_t               *titlebar;
    lv_obj_t               *title_label;
    lv_obj_t               *close_btn;
    lv_obj_t               *content;     /* the app's area */

    /* WM-authoritative geometry; LVGL is a projection of this */
    lv_area_t               geom;
    char                    title[ZD_TITLE_MAX];

    /* state */
    bool                    focused;
    bool                    mapped;
    bool                    pending_destroy;

    /* ownership */
    struct zd_app_instance *owner;       /* NULL == desktop-internal window */

    /* transient drag state */
    lv_point_t              drag_grab;   /* pointer pos at press */
    lv_point_t              drag_origin; /* window pos at press */
    bool                    dragging;
};

struct zd_wm {
    lv_obj_t          *layer_desktop, *layer_windows, *layer_panel;
    sys_dlist_t        stack;            /* z-order, front to back */
    struct zd_client  *focused;
    uint32_t           next_id;
    struct zd_session *session;
    struct k_work      reap_work;        /* deferred destruction — see 3.4 */
    sys_slist_t        reap_list;
};
```

The `sys_dlist_t` is the truth. `zd_wm_restack()` re-applies list order onto LVGL. Keeping
the model authoritative rather than reading z-order back out of LVGL is what makes
always-on-top, minimize, and per-app window groups cheap later.

Clients are allocated from a fixed `K_MEM_SLAB` (`ZD_MAX_CLIENTS`, start at 8), not the
heap — bounded, and a leak shows up immediately as slab exhaustion, which matters for the
"no leaks" success criterion.

### 3.3 Event loop and dispatch

LVGL supplies the input device and the hit test; the WM supplies the policy. Two rules
keep this from degenerating into scattered callbacks:

1. **Every client frame gets exactly one WM callback**:
   `lv_obj_add_event_cb(frame, zd_wm_frame_event, LV_EVENT_ALL, client)`, with
   `LV_OBJ_FLAG_EVENT_BUBBLE` on children. Any press anywhere inside a window bubbles to
   the frame, so `raise + focus` is one code path regardless of what was clicked. The
   event then continues to the widget that was actually hit.
2. **Drag is implemented by the WM, not by LVGL's built-in dragging.** On titlebar
   `LV_EVENT_PRESSED`, record `lv_indev_get_point()` and `client->geom`; on
   `LV_EVENT_PRESSING`, apply the delta via `lv_obj_set_pos()` and update `geom`; on
   `LV_EVENT_RELEASED`, clear. This keeps hit-test → focus → dispatch a single WM-owned
   pipeline, so a future keyboard/encoder/touch modality routes identically instead of
   inheriting LVGL widget behaviour.

The desktop thread:

```c
for (;;) {
    zd_wm_reap();                 /* deferred destroys, outside dispatch */
    lvgl_lock();
    uint32_t sleep = lv_timer_handler();
    lvgl_unlock();
    k_msleep(MIN(sleep, ZD_TICK_MAX_MS));
}
```

`lvgl_lock()`/`lvgl_unlock()` are the module's own helpers (§1.5) and are the only
sanctioned cross-thread entry. Any host-API call reaching LVGL takes them.

### 3.4 Deferred destruction — the thing most likely to crash

An app that calls `zd_window_close()` from inside its own event callback is, transitively,
inside `lv_timer_handler()`, on a stack frame that lives in the extension's text. Deleting
the LVGL subtree there, or worse `llext_unload()`ing there, is a use-after-free.

Therefore: **nothing is ever destroyed during dispatch.** `zd_window_close()` sets
`pending_destroy`, unlinks the client from focus/stack, and pushes it onto `reap_list`.
`zd_wm_reap()` runs at the top of the loop and does the `lv_obj_delete()`. Instance
teardown (`fini` → `llext_teardown` → `llext_unload`) is a second reap stage that only
runs once the instance's window count reaches zero *and* no app frame is on the stack
(tracked with a simple `in_app_callback` depth counter). Get this right once, in one file.

### 3.5 Retro chrome

Hand-built, desktop-internal, in `app/src/chrome/`. Not `lv_win` — `lv_win` gives a header
container but its own layout assumptions get in the way of pixel-exact bevels, and the WM
needs to own the titlebar's event handling anyway. Use plain `lv_obj_t` containers with:

- Bevels as 2px borders in a fixed 4-colour palette (`ZD_C_FACE`, `ZD_C_LIGHT`,
  `ZD_C_SHADOW`, `ZD_C_DARK`) applied via reusable `lv_style_t` objects — one
  `zd_style_bevel_out` / `zd_style_bevel_in`, shared by every window and button.
- Titlebar: active/inactive fill styles swapped on focus change, `lv_label` title, and a
  close button that is a bevelled `lv_button` with a drawn ✕.
- Background: an `lv_canvas`-drawn or `lv_style_bg_image` tiled 8×8 dither pattern.

All of this stays behind LVGL's draw layer — no direct framebuffer access anywhere, so
adopting the PXP draw unit later is a Kconfig change.

---

## 4. Architecture — app ABI

### 4.1 Shape (per your decisions)

- **Host-API vtable.** The desktop exports exactly one symbol; the app receives a
  `const struct zd_host_api *` at init. Versionable, keeps the export surface at ~1
  symbol, and gives every call a natural place to hang the per-instance permission check.
- **Opaque handles now, versioned escape hatch later.** Apps see `zd_window_t` /
  `zd_label_t`, never `lv_obj_t`. A reserved `unsafe_lvgl_content` slot exists in the
  vtable from day one but is **NULL unless the app manifest is flagged trusted**, so the
  contract is written down before it's needed and cannot be accidentally relied upon.
- Apps export their manifest via `LL_EXTENSION_SYMBOL` — that is the llext-native
  direction and needs no vtable.

`include/zd/app_abi.h` is the whole contract, and includes **no Zephyr and no LVGL
headers** — only `stdint.h`/`stddef.h`. That is what makes an alternate link mode possible
later, and what stops LVGL's ABI from silently becoming ours.

### 4.2 The contract

```c
#define ZD_ABI_MAJOR 0
#define ZD_ABI_MINOR 1

typedef struct zd_app_ctx *zd_app_ctx_t;   /* opaque, per instance */
typedef struct zd_window  *zd_window_t;
typedef struct zd_label   *zd_label_t;
typedef struct zd_file    *zd_file_t;

struct zd_rect { int16_t x, y, w, h; };

enum zd_event_type {
    ZD_EV_WINDOW_SHOWN, ZD_EV_WINDOW_CLOSE_REQUEST,
    ZD_EV_WINDOW_FOCUS, ZD_EV_WINDOW_BLUR,
    ZD_EV_CLICK,                     /* content-area click, window-relative */
    ZD_EV_KEY,                       /* reserved; not delivered in MVP */
};

struct zd_event {
    enum zd_event_type type;
    zd_window_t        win;
    union { struct { int16_t x, y; } click; struct { uint32_t code; } key; };
};

struct zd_window_desc {
    const char    *title;
    struct zd_rect geom;             /* w/h == 0 → desktop picks + cascades */
    uint32_t       flags;
};

enum zd_dir { ZD_DIR_HOME, ZD_DIR_SYSTEM_APPS, ZD_DIR_USER_APPS, ZD_DIR_TMP };

struct zd_host_api {
    uint16_t abi_major, abi_minor;
    uint32_t struct_size;            /* additive-growth guard */

    /* windows */
    zd_window_t (*window_create)(zd_app_ctx_t, const struct zd_window_desc *);
    void        (*window_close)(zd_window_t);
    void        (*window_set_title)(zd_window_t, const char *);
    void        (*window_set_geometry)(zd_window_t, const struct zd_rect *);
    void        (*window_get_geometry)(zd_window_t, struct zd_rect *);

    /* content — deliberately tiny; grows one widget at a time, on demand */
    zd_label_t  (*label_create)(zd_window_t, const char *text, int16_t x, int16_t y);
    void        (*label_set_text)(zd_label_t, const char *text);

    /* filesystem — always ctx-scoped, never raw paths (see §5) */
    int  (*path_resolve)(zd_app_ctx_t, enum zd_dir, char *out, uint32_t out_len);
    int  (*fs_open)(zd_app_ctx_t, const char *path, uint32_t flags, zd_file_t *out);
    int  (*fs_read)(zd_file_t, void *buf, uint32_t len);
    int  (*fs_write)(zd_file_t, const void *buf, uint32_t len);
    void (*fs_close)(zd_file_t);
    int  (*fs_opendir)(zd_app_ctx_t, const char *path, zd_dir_t *out);
    int  (*fs_readdir)(zd_dir_t, struct zd_dirent *out);
    void (*fs_closedir)(zd_dir_t);

    /* misc */
    void    (*log)(zd_app_ctx_t, int level, const char *msg);
    int64_t (*uptime_ms)(void);

    /* trusted-only escape hatch; NULL for untrusted apps. Returns lv_obj_t*. */
    void   *(*unsafe_lvgl_content)(zd_window_t);
};

/* Exported by the DESKTOP, resolved by the loader — one symbol. */
const struct zd_host_api *zd_get_host_api(void);

/* Exported by each APP. */
#define ZD_APP_MAGIC 0x5A444150u /* 'ZDAP' */
#define ZD_APP_FLAG_TRUSTED      BIT(0)
#define ZD_APP_FLAG_SINGLETON    BIT(1)
#define ZD_APP_FLAG_WANTS_THREAD BIT(2)   /* honoured post-MVP; see 4.4 */

struct zd_app_manifest {
    uint32_t    magic;
    uint16_t    abi_major, abi_minor;
    uint32_t    flags;
    const char *name;
    const char *icon;                       /* NULL in MVP */
    int  (*init) (zd_app_ctx_t, const struct zd_host_api *);
    void (*event)(zd_app_ctx_t, const struct zd_event *);
    void (*fini) (zd_app_ctx_t);
};
```

Desktop side: `EXPORT_GROUP_SYMBOL(DESKTOP, zd_get_host_api);` with
`CONFIG_LLEXT_EXPORT_SYMBOL_GROUP_DESKTOP=y` in the desktop's own Kconfig.
App side: `LL_EXTENSION_SYMBOL(zd_app_manifest);`.

**Versioning rule, written into `docs/abi.md` on day one:** `abi_major` must match
exactly; `app.abi_minor <= host.abi_minor` is accepted; the vtable only ever grows by
appending, and `struct_size` lets an older app safely bind against a newer host. Anything
else is a major bump.

### 4.3 Handle safety

`zd_window_t` is not a raw `struct zd_client *`. It is a pointer into a WM-owned registry
entry `{ uint32_t generation; struct zd_client *client; }`. Every host-API call runs
`zd_handle_deref()`, which validates that the entry is live, that the generation matches,
and **that the client's `owner` is the calling `ctx`'s instance**. A stale or forged handle
gets `-EINVAL`, not a fault, and app A cannot manipulate app B's windows. This is cheap,
and it is the only part of the isolation story that actually works without an MMU.

### 4.4 Lifecycle

```
discover  scan /system/apps and <home>/apps for *.llext  → launcher entries
   ↓ (user clicks)
load      struct llext_fs_loader l = LLEXT_FS_LOADER(path);
          llext_load(&l.loader, name, &ext, &param)
   ↓
bringup   llext_bringup(ext)                          /* .init_array */
   ↓
bind      llext_find_sym(&ext->exp_tab, "zd_app_manifest")
          validate magic, abi_major ==, abi_minor <=
   ↓
instance  alloc zd_app_instance { ext, manifest, session, windows, id }
          alloc zd_app_ctx bound to it
   ↓
init      manifest->init(ctx, host_api)               /* desktop thread, lvgl_lock held */
   ↓
run       manifest->event(ctx, ev) from WM dispatch   /* callback-driven, no app thread */
   ↓
teardown  close all owned windows (deferred, §3.4)
          manifest->fini(ctx)
          llext_teardown(ext)                         /* .fini_array */
          llext_unload(&ext)
```

**MVP is callback-driven, single-threaded.** Hello world needs no thread, and
thread-per-app is precisely where the ABI gets hard (locking discipline, priorities,
per-thread teardown). But the ABI does not preclude it: `ZD_APP_FLAG_WANTS_THREAD` is
defined now and documented as "reserved — the desktop will use `llext_bootstrap()` with a
per-instance stack." The terminal will need it; hello world will not; the contract
doesn't change when it arrives.

### 4.5 Surviving a misbehaving app — stated honestly

On this target, without `CONFIG_USERSPACE`, **a loaded llext is trusted code sharing the
kernel address space.** A wild pointer takes down the system. What the MVP actually
provides, and what it does not:

*Genuinely enforced:*
- Handle validation with generation counters + owner checks (§4.3) — no crash from stale
  or cross-app handles.
- Per-instance window quota (`ZD_MAX_WINDOWS_PER_APP`) and a global client slab cap.
- Path scoping in the fs shim (§5) — every path normalized, `..` rejected, checked
  against the session's roots.
- ABI version gate at load; a mismatched app is rejected before `init` runs.
- Bounded teardown: an instance that fails `init` is unloaded immediately; an app whose
  windows are all closed is reaped.

*Not enforced, and the plan says so out loud:*
- Memory safety. No MMU/MPU isolation in the MVP.
- An app calling Zephyr APIs directly, bypassing the shim entirely. **Mitigation available
  now and worth taking:** set `CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n` and hand-export only
  what apps legitimately need. That does not make the shim a security boundary, but it
  makes it the only *linkable* route, which is the difference between a contract and a
  suggestion. (Expect to re-add a handful of libc symbols; budget a task for it.)
- Infinite loops / blocking in an app callback. The desktop thread hangs. A watchdog that
  can actually kill an app requires the app to have its own thread — deferred with
  `ZD_APP_FLAG_WANTS_THREAD`.

*Future hardening path, already reachable on both targets:* `CONFIG_USERSPACE` +
`llext_add_domain(ext, &domain)`, with `ARCH_HAS_USERSPACE` available on arm64 via
`ARM_MMU` and on the RT1060's Cortex-M7 via `ARM_MPU`. Deferred, but not designed out —
this is why every host-API call already carries a `zd_app_ctx_t`: those calls become
syscalls without changing a single app.

---

## 5. Session, users, permissions

```c
struct zd_session {
    uint32_t uid;
    char     user[ZD_USER_MAX];   /* "user" */
    char     home[ZD_PATH_MAX];   /* "/home/user" */
    uint32_t caps;                /* ZD_CAP_FS_SYSTEM_RO | ZD_CAP_FS_HOME_RW | ... */
};
```

Exactly one session is created at boot. **No global "current user" anywhere.** Every
host-API entry takes `zd_app_ctx_t`; the ctx points at its instance, the instance points
at its session. `path_resolve(ctx, ZD_DIR_HOME, ...)` reads `ctx->inst->session->home`.
The WM also carries `session` so window titles/menus can be per-session later. Adding a
second session is then "construct another `zd_session`, hand it to new instances" — a
login screen, not a refactor.

`app/src/host/fs_shim.c` is the single choke point: normalize → reject `..` and symlink
escapes → match against the session's permitted roots and their read/write mode → call
Zephyr `fs_*`. Permissions are **filesystem-level and advisory**, enforced only because
the shim is the only exported route (§4.5). No hardware enforcement. Say this in
`docs/design.md` too, not just here.

**Filesystem layout** (identical on QEMU and hardware; only the mount table differs):

```
/system/apps/        *.llext, system-installed        app-visible: read-only
/system/share/       fonts, wallpaper, desktop assets app-visible: read-only
/home/user/          the session's home               read-write
/home/user/apps/     user-installed *.llext           read-write, also enumerated
/tmp/                scratch                          read-write
```

QEMU: FAT over `zephyr,ram-disk` (`disk-name = "RAM"`, 512 B × 4096 = 2 MB),
`FS_MOUNT_FLAG_USE_DISK_ACCESS`, `CONFIG_FS_FATFS_MKFS` to format on first boot.
RT1060: FAT over `zephyr,sdmmc-disk` on `usdhc1`, same paths.

**Getting `.llext` files onto the QEMU FS:** `add_llext_target` emits `hello.llext` into
the build dir; `generate_inc_file_for_target()` embeds it; `zd_seed_install()` writes it
to `/system/apps/hello.llext` at boot if absent. The discover→open→`llext_fs_loader` path
is 100% real — only the delivery is synthetic, and it mirrors what an installer does. When
apps move out-of-tree (EDK), swap the seed for the fw_cfg channel (§1.6) and the desktop
image stops needing a rebuild per app.

---

## 6. Repo and build layout

```
zephyr-desktop/
  west.yml                       # T2 topology, imports zephyr @ pinned main commit
  .gitignore                     # ignore the west-managed zephyr/ and modules/
  CLAUDE.md
  docs/
    design.md                    # this doc, trimmed to what's true
    abi.md                       # the ABI contract + versioning rules
  include/zd/
    app_abi.h                    # THE contract; no Zephyr, no LVGL includes
    version.h
  app/                           # the desktop image (the Zephyr application)
    CMakeLists.txt               # also drives add_llext_target for each app
    Kconfig                      # ZD_* options incl. EXPORT_SYMBOL_GROUP_DESKTOP
    prj.conf
    boards/
      qemu_cortex_a53.conf
      qemu_cortex_a53.overlay    # ramdisk node, ramfb 480x272, icount off
      mimxrt1060_evk.conf        # milestone G
      mimxrt1060_evk.overlay
    src/
      main.c                     # mount fs → seed → session → wm → lvgl loop
      wm/       wm.c wm.h client.c stack.c focus.c drag.c handle.c
      chrome/   theme.c theme.h bevel.c titlebar.c
      shell/    desktop.c taskbar.c launcher.c clock.c
      host/     host_api.c fs_shim.c session.c
      loader/   app_loader.c app_instance.c seed.c
  apps/
    hello/hello.c                # one file — LLEXT_TYPE_ELF_OBJECT allows only one
    hello2/hello2.c              # second trivial app, proves multi-instance/multi-app
```

Notes:

- **`west.yml` in the project root, T2 star topology**, `import`ing upstream Zephyr's
  manifest at a pinned `main` commit ≥ 2026-06-15 (`e201b84b04e4` verified). Workspace is
  a *fresh* `west init -l`, separate from `really-native-sim/` — that tree carries your
  portability branches and must not be entangled.
- Apps are built by the desktop's CMake via `add_llext_target` + `llext_include_directories(... ${CMAKE_CURRENT_SOURCE_DIR}/../include)`,
  but they are **separate ELF artifacts** from `zephyr.elf` from the first commit. One
  `west build` produces `zephyr.elf` and `hello.llext`. The migration to genuinely
  out-of-tree app builds is `west build -t llext-edk` + an independent CMake project;
  keeping `include/zd/` outside `app/` is what makes that a move, not a rewrite.
- One source file per app is a hard constraint under `LLEXT_TYPE_ELF_OBJECT` (ARM/ARM64
  default). Fine for the MVP; switching to `LLEXT_TYPE_ELF_RELOCATABLE` lifts it later.

Key `prj.conf` content: `CONFIG_LVGL=y`, `CONFIG_LV_Z_LVGL_MUTEX=y`,
`CONFIG_LV_Z_POINTER_FROM_CHOSEN_TOUCH=y`, `CONFIG_DISPLAY=y`, `CONFIG_INPUT=y`,
`CONFIG_LLEXT=y`, `CONFIG_LLEXT_HEAP_SIZE=128`, `CONFIG_FILE_SYSTEM=y`,
`CONFIG_FAT_FILESYSTEM_ELM=y`, `CONFIG_FS_FATFS_MKFS=y`, `CONFIG_DISK_DRIVER_RAM=y`,
plus `CONFIG_LLEXT_EXPORT_SYMBOL_GROUP_DESKTOP=y` (defined in `app/Kconfig`).

---

## 7. Ordered task list

Each milestone ends in something you can look at.

### A — Workspace and blank desktop — **DONE**
1. ✅ `west init -l manifest`, Zephyr pinned to `main` @ `e201b84b`, LVGL 9.6.0-dev
   confirmed. The manifest lives in `manifest/` rather than the repo root so that the
   west topdir can *be* the project repo; `west init -l` does not require that directory
   to be its own git repo.
2. ✅ **Gate passed.** Stock `samples/modules/lvgl/demos` renders at 480×272 into ramfb,
   and injected virtio-tablet events switch the demo's tab — pointer motion and clicks
   reach LVGL with correct coordinate mapping. Verified headlessly over QMP rather than
   by eye; `tools/shot.py` is the harness, and every later milestone reuses it. The one
   piece verified structurally rather than visually is the cocoa window itself: the
   generated command line is `-device ramfb -vga none -display cocoa,show-cursor=on`,
   and this machine's QEMU 10.0.2 reports `cocoa` among its display backends.
3. ✅ `app/` skeleton, `prj.conf`, board conf/overlay. 984 KB RAM, clean boot.
4. ✅ `chrome/theme.c` + `shell/desktop.c`. Bevel verified by pixel dump, not by eye:
   `#FFFFFF` outer ring, `#DFDFDF` inner, `#C0C0C0` face.

**[A] Two corrections to this document, found by building it:**

- `lv_obj_move_foreground()` exists **only** in LVGL's v8 compatibility shim
  (`api_map/lv_api_map_v8.h`), which is not compiled in. §3.1's raise operation is
  `lv_obj_move_to_index(obj, idx)`. This is the better primitive anyway: `zd_wm_restack()`
  wants to write absolute indices straight from the dlist, not nudge one object at a time.
- `CONFIG_MAX_XLAT_TABLES` defaults to 8 and LVGL alone already warns `xlat tables low:
  7 of 8 in use` at boot. Raised to 16 in the board conf, before the llext heap and app
  instances need their own mappings.
- Bevels are drawn in `LV_EVENT_DRAW_POST`, not expressed as border styles. An LVGL style
  carries a single border colour, so a two-tone Win95 edge would otherwise need a nested
  object per bevel. Drawing keeps one object per visual element and stays behind the draw
  layer, so a GPU draw unit still applies.

### B — Menu bar and clock — **DONE**
5. ✅ `shell/taskbar.c`: raised "Start" button, sunken clock, `lv_timer` tick. The
   launcher is inert but already routes clicks, which is what proves pointer input
   reaches the panel layer.

**[B]** The board has no RTC node and Zephyr has no PL031 driver, so there is no wall
clock to read. The clock counts up from a fixed 9:41 rather than from zero — a display
counting from 00:00 reads as a stopwatch, not a desktop. One function to swap for
`rtc_get_time()` on hardware.

**[B]** Press feedback silently did nothing at first: LVGL only invalidates on a state
change when a *style* property depends on that state, and these bevels are draw
callbacks. `zd_bevel_attach()` now requests the redraw explicitly on
PRESSED/RELEASED/PRESS_LOST. Anything else state-dependent and custom-drawn needs the
same treatment.

### C — One hardcoded window with decorations — **DONE**
6. ✅ `wm/wm.c` + `wm/client.c`, client slab, full chrome subtree.
7. ✅ `chrome/titlebar.c`: active/inactive styles, close glyph drawn pixel by pixel and
   nudged down-right while pressed, as Win95 does.
8. ✅ Hardcoded window at boot.

### D — Drag, raise, focus — **DONE**
9. ✅ `wm/stack.c`: dlist z-order + `zd_wm_restack()` via `lv_obj_move_to_index()`,
   walking bottom-to-top with ascending indices so the moves do not fight each other.
10. ✅ `wm/focus.c`: one `frame_event` per frame, `EVENT_BUBBLE` on every child, press →
    raise + focus, titlebar recolour, background press → defocus. Closing the focused
    window hands focus to the new topmost rather than leaving the desktop blank.
11. ✅ `wm/drag.c`: WM-owned drag with clamping that always keeps 60px of titlebar
    reachable, so a window can never be stranded off-edge.
12. ✅ Deferred reap verified end to end. Console shows `queued for reap`, then
    `reaping`, then `reaped 1 window(s); 1 live, 7 slab blocks free` — slab accounting
    exactly balanced.

**[D] Reordering:** `wm/handle.c` (the generation-counted handle registry) moved to
milestone F. It exists to validate handles crossing the app ABI, and there are no app
handles until F; building it here would have been speculative. The deferred-reap half of
task 12, which is the part with real risk, landed at C and is verified here.

**[D]** A full N-cycle create/close leak assertion still wants an external trigger, so it
lands at F task 24 where the launcher can spawn on demand. The single-cycle slab
accounting is verified.

### E — Filesystem and discovery — **DONE**
13. ✅ 2 MB `zephyr,ram-disk`, FAT, auto-format on first mount, full directory layout.
14. ✅ `host/session.c` + `host/storage.c`.
15. ✅ `host/fs_shim.c`.
16. ✅ `loader/app_loader.c` discovery half.
17. ✅ `shell/launcher.c`: Start menu listing discovered apps; picking one opens a window
    named after it. Verified — the menu shows `hello` and `notes`, both read from the
    filesystem, and the rescan happens on every open so dropping a file in changes what
    the menu shows.

**[E] Paths are not free-form.** FATFS requires a mount point of the form `/<VOLUME>:`,
with the volume string generated from the devicetree `disk-name`, so the root is `/RAM:`
under QEMU and would be an SD volume on hardware. §5's `/system/apps` and `/home/user`
are therefore *relative to* `CONFIG_ZD_FS_ROOT`, not absolute. This costs nothing because
apps never build paths themselves — they resolve directories through the session — which
is precisely the indirection §5 already called for, now load-bearing rather than
decorative.

**[E] FAT needs long filenames.** Without `CONFIG_FS_FATFS_LFN`, FAT is limited to 8.3,
which caps extensions at three characters — and every app binary ends in `.llext`. Set
alongside `CONFIG_FS_FATFS_MAX_LFN=64`.

**[E]** The two `.llext` files present at this milestone are placeholders written at boot,
not valid ELF, and are never loaded. What is real is the path: opendir, readdir, suffix
match, menu. Milestone F replaces them with a genuine build artifact.

### F — llext load/unload of hello world *(demo: the actual success criterion)*
18. `include/zd/app_abi.h` — the full contract from §4.2. Write `docs/abi.md` alongside it.
19. `host/host_api.c`: the vtable, `zd_get_host_api()`, `EXPORT_GROUP_SYMBOL(DESKTOP, ...)`,
    `CONFIG_LLEXT_EXPORT_SYMBOL_GROUP_DESKTOP` in `app/Kconfig`.
20. Handle registry with generation counters + owner checks (§4.3).
21. `apps/hello/hello.c`: manifest via `LL_EXTENSION_SYMBOL`, `init` creates a window and
    a label. `add_llext_target` + `llext_include_directories` in `app/CMakeLists.txt`.
22. `loader/seed.c`: `generate_inc_file_for_target` embed + write to `/system/apps` at
    boot if absent.
23. `loader/app_instance.c`: the full lifecycle (§4.4) — load, bringup, bind, validate
    ABI, init, event dispatch, deferred teardown, `llext_unload`.
24. Launcher click → spawn. Close → unload. **Instrument slab and llext heap free-space
    before/after a spawn-close cycle and assert they return to baseline.**

### G — Two instances, and hardening *(demo: the full criterion, plus honesty)*
25. `apps/hello2/hello2.c`; multi-instance of the same app with cascade placement;
    per-instance window quota.
26. Try `CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n`; re-export only what hello world genuinely
    needs. Record the resulting symbol list in `docs/abi.md`.
27. Negative tests: ABI-major mismatch rejected; bad magic rejected; stale handle after
    close returns `-EINVAL`; path escape via `../..` rejected; window quota enforced.

### H — Hardware checkpoint, headless *(demo: same binary spine on a Cortex-M7)*
28. `mimxrt1060_evk` board conf/overlay: `CONFIG_DUMMY_DISPLAY` + `zephyr,dummy-dc`,
    FAT on SD via `zephyr,sdmmc-disk`.
29. Copy `hello.llext` onto an SD card by hand — the first time a `.llext` arrives as a
    genuinely external file.
30. Boot, verify discovery + load + window-create + unload over the console log. Record
    RAM/flash cost and `llext` heap high-water. **Panel purchase decision point:** with an
    `rk043fn66hs_ctg`, this milestone becomes the graphical desktop at the same 480×272,
    and `CONFIG_LV_USE_PXP=y` becomes a one-line accel experiment.

---

## 8. Explicitly deferred

Named so they don't leak into the MVP: any catalog app (file browser, text editor, image
viewer, media player, terminal, web server, web browser); hardware acceleration (PXP
exists and is one Kconfig away — not now); MCU→MPU responsive layout morph; **window
resize**; IPC and desktop services (clipboard, notifications); multiple displays;
hardware-enforced isolation (`USERSPACE` + `llext_add_domain` + memory domains); a theming
engine (MVP hardcodes one palette); sound; real multi-user login; thread-per-app
(`ZD_APP_FLAG_WANTS_THREAD` is reserved, not honoured); out-of-tree app builds via the
llext EDK; the fw_cfg app-delivery channel; keyboard input; app icons; `native_sim`.

---

## 9. Risks and open questions

**Risks I'd watch, roughly in order:**

1. ~~**Milestone A step 2 is a true gate.**~~ **[A] Retired.** ramfb + virtio-tablet render
   and route input correctly on this host. The `qemu_x86` fallback is no longer needed.
2. **Deferred destruction (§3.4).** Now the top risk. The single most likely source of
   hard-to-debug faults.
   It is why it gets its own file and its own tasks rather than being sprinkled around.
3. **QEMU + TCG + software rendering interactivity.** `CONFIG_QEMU_ICOUNT=y` is on by
   default for this board and may make dragging feel wrong. **[A]** Turned off in
   `app/boards/qemu_cortex_a53.conf`, but not yet judged against an interactive pointer —
   there is nothing draggable until milestone D.
4. **llext heap sizing under MMU.** Zephyr's own arm64 llext tests bump
   `CONFIG_LLEXT_HEAP_SIZE=128`; expect tuning, and expect the failure mode to be a
   confusing `llext_load` error rather than an obvious OOM.
5. **Turning off default export groups (task 26) may cascade.** Hello world links against
   more libc than you'd guess. Timeboxed; if it fights back, leave the groups on and
   document the gap honestly rather than burning the milestone.
6. **One source file per app** under `LLEXT_TYPE_ELF_OBJECT`. Invisible for hello world,
   an immediate wall for the text editor. Switching to `LLEXT_TYPE_ELF_RELOCATABLE` should
   be tried once, early, just to know it works.

**Where I'd want your input, when we reach it:**

- **Task 26 outcome** — if narrowing the export surface proves painful, is the honest
  "shim is a contract, not a boundary" note sufficient for you, or do you want the
  surface narrow even at the cost of app ergonomics?
- **Milestone H, step 30** — whether to buy the `rk043fn66hs_ctg` before or after the
  headless hardware checkpoint. Cheap either way; it just reorders the fun.
- **First post-MVP app.** The terminal forces `WANTS_THREAD`; the file browser forces the
  fs ABI and a list widget; the text editor forces keyboard input and multi-file
  extensions. Whichever you pick first determines which ABI extension gets designed next,
  and I'd rather know than guess.

## 10. Verification

- **Per milestone:** `west build -b qemu_cortex_a53 app && west build -t run`, and look at
  the cocoa window. Every milestone A–G is demoable this way.
- **The success criterion, end to end (after G):** boot → retro desktop with patterned
  background, taskbar, live clock → click launcher → menu lists `hello` and `hello2`
  discovered from `/system/apps` → click `hello` twice → two windows, cascaded, retro
  chrome → drag one over the other by its titlebar, click each to raise → correct z-order
  and titlebar focus styling → close both via the close button → console shows
  `llext_unload` for each, and the instrumented client-slab and llext-heap counters return
  to their pre-spawn values.
- **Leak check** is an explicit assertion in task 24, not an eyeball: capture slab
  in-use count and `llext` heap free bytes at boot, after N spawn/close cycles, and
  compare.
- **Negative tests** (task 27) are run manually against a deliberately broken app built
  with a bumped `ZD_ABI_MAJOR` and a hand-corrupted magic.
- **Hardware (H):** console log only — discovery, load, window create, unload — plus
  recorded RAM/flash footprint.
