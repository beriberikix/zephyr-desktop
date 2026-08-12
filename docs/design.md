# zephyr-desktop MVP — design doc + task list

> **Status: MVP complete; storage added post-MVP (ABI 0.3, milestone I), then window
> management round 2 (ABI 0.4, milestone J) — resize, minimise, a taskbar window list and
> the close handshake.** Milestones A-G are done and verified on `qemu_cortex_a53`;
> milestone H builds and is documented, with the on-board run left for you (see
> `docs/hardware.md`) since it needs the EVK in hand. Workspace pinned, target strategy proven end to end,
> retro theme, taskbar, and a full stacking WM with drag, focus and deferred destruction
> running on `qemu_cortex_a53`, plus a FAT filesystem, session, path-scoping shim and a
> launcher that lists zapps found on disk, and a real .llext zapp loaded at runtime that
> opens a window and draws "hello world", two live instances of it, and a one-symbol
> export surface. See §7 for what each milestone actually produced. Two findings from building A are folded in below, marked **[A]**.

## Context

Build the *spine* of a retro desktop shell on Zephyr + LVGL: overlapping draggable
windows with hand-built Win95/System-7 chrome, a taskbar with launcher and clock, and
zapps that are real `.llext` binaries discovered on a filesystem and loaded at runtime.
The MVP is a scaffolding proof, not a pixel proof — success is "boot to desktop, launcher
finds zapps on disk, click loads a `.llext`, zapp calls the desktop ABI to open a window
saying hello, two instances drag over each other with correct z-order and focus, closing
unloads the extension with no leaks."

Two pieces are load-bearing and get real design attention: the **WM data model + event
loop** (modelled on a tiny X11 stacking WM) and the **zapp ABI** (designed as if the
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
  `k_thread_create`-compatible signature for the future thread-per-zapp case.
- Symbol export, desktop → zapp: `EXPORT_SYMBOL(x)`, `EXPORT_SYMBOL_NAMED`,
  `EXPORT_GROUP_SYMBOL(GROUP, x)` (`include/zephyr/llext/symbol.h`). Groups are gated by
  `CONFIG_LLEXT_EXPORT_SYMBOL_GROUP_<GROUP>`.
- Symbol export, zapp → desktop: `LL_EXTENSION_SYMBOL(x)`, looked up in `ext->exp_tab`.
- **`CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=y` by default**, which turns on `UNASSIGNED`,
  `SYSCALL` and `LIBC` groups. This matters for the permission model (§5).
- Build tooling: `add_llext_target(<name> OUTPUT <file.llext> SOURCES <src>)` plus
  `llext_include_directories()` / `llext_compile_options()`
  (`cmake/modules/extensions.cmake:6109`). Runs inside the Zephyr zapp build, emits a
  separate `.llext` artifact. ARM/ARM64 default to `LLEXT_TYPE_ELF_OBJECT`, which allows
  **exactly one source file per extension**. True out-of-tree zapp builds later via
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
- `lv_timer_handler()` is called by the zapp's loop (`modules/lvgl/lvgl.c:244` shows the
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
second link mode, no static-linked-zapp fallback, no Linux VM, and no risk of the zapp
model forking. That is a strict simplification of the original plan, not a compromise.

Consequences worth naming:

- `native_sim` is **dropped from the MVP entirely**, not deferred-with-a-hook. It cannot
  load extensions, so having it would immediately force the static-link fork you wanted
  to avoid. If it comes back later purely as a fast pixel-iteration target, it comes back
  behind the same ABI header with a `zd_zapp_register_static()` shim — but nothing in the
  MVP is shaped around that possibility beyond keeping `include/zd/zapp_abi.h` free of
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
    struct zd_zapp_instance *owner;       /* NULL == desktop-internal window */

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
always-on-top, minimize, and per-zapp window groups cheap later.

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

A zapp that calls `zd_window_close()` from inside its own event callback is, transitively,
inside `lv_timer_handler()`, on a stack frame that lives in the extension's text. Deleting
the LVGL subtree there, or worse `llext_unload()`ing there, is a use-after-free.

Therefore: **nothing is ever destroyed during dispatch.** `zd_window_close()` sets
`pending_destroy`, unlinks the client from focus/stack, and pushes it onto `reap_list`.
`zd_wm_reap()` runs at the top of the loop and does the `lv_obj_delete()`. Instance
teardown (`fini` → `llext_teardown` → `llext_unload`) is a second reap stage that only
runs once the instance's window count reaches zero *and* no zapp frame is on the stack
(tracked with a simple `in_zapp_callback` depth counter). Get this right once, in one file.

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

## 4. Architecture — zapp ABI

### 4.1 Shape (per your decisions)

- **Host-API vtable.** The desktop exports exactly one symbol; the zapp receives a
  `const struct zd_host_api *` at init. Versionable, keeps the export surface at ~1
  symbol, and gives every call a natural place to hang the per-instance permission check.
- **Opaque handles now, versioned escape hatch later.** Zapps see `zd_window_t` /
  `zd_label_t`, never `lv_obj_t`. A reserved `unsafe_lvgl_content` slot exists in the
  vtable from day one but is **NULL unless the zapp manifest is flagged trusted**, so the
  contract is written down before it's needed and cannot be accidentally relied upon.
- Zapps export their manifest via `LL_EXTENSION_SYMBOL` — that is the llext-native
  direction and needs no vtable.

`include/zd/zapp_abi.h` is the whole contract, and includes **no Zephyr and no LVGL
headers** — only `stdint.h`/`stddef.h`. That is what makes an alternate link mode possible
later, and what stops LVGL's ABI from silently becoming ours.

### 4.2 The contract

```c
#define ZD_ABI_MAJOR 0
#define ZD_ABI_MINOR 1

typedef struct zd_zapp_ctx *zd_zapp_ctx_t;   /* opaque, per instance */
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

enum zd_dir { ZD_DIR_HOME, ZD_DIR_SYSTEM_ZAPPS, ZD_DIR_USER_ZAPPS, ZD_DIR_TMP };

struct zd_host_api {
    uint16_t abi_major, abi_minor;
    uint32_t struct_size;            /* additive-growth guard */

    /* windows */
    zd_window_t (*window_create)(zd_zapp_ctx_t, const struct zd_window_desc *);
    void        (*window_close)(zd_window_t);
    void        (*window_set_title)(zd_window_t, const char *);
    void        (*window_set_geometry)(zd_window_t, const struct zd_rect *);
    void        (*window_get_geometry)(zd_window_t, struct zd_rect *);

    /* content — deliberately tiny; grows one widget at a time, on demand */
    zd_label_t  (*label_create)(zd_window_t, const char *text, int16_t x, int16_t y);
    void        (*label_set_text)(zd_label_t, const char *text);

    /* filesystem — always ctx-scoped, never raw paths (see §5) */
    int  (*path_resolve)(zd_zapp_ctx_t, enum zd_dir, char *out, uint32_t out_len);
    int  (*fs_open)(zd_zapp_ctx_t, const char *path, uint32_t flags, zd_file_t *out);
    int  (*fs_read)(zd_file_t, void *buf, uint32_t len);
    int  (*fs_write)(zd_file_t, const void *buf, uint32_t len);
    void (*fs_close)(zd_file_t);
    int  (*fs_opendir)(zd_zapp_ctx_t, const char *path, zd_dir_t *out);
    int  (*fs_readdir)(zd_dir_t, struct zd_dirent *out);
    void (*fs_closedir)(zd_dir_t);

    /* misc */
    void    (*log)(zd_zapp_ctx_t, int level, const char *msg);
    int64_t (*uptime_ms)(void);

    /* trusted-only escape hatch; NULL for untrusted apps. Returns lv_obj_t*. */
    void   *(*unsafe_lvgl_content)(zd_window_t);
};

/* Exported by the DESKTOP, resolved by the loader — one symbol. */
const struct zd_host_api *zd_get_host_api(void);

/* Exported by each APP. */
#define ZD_ZAPP_MAGIC 0x5A444150u /* 'ZDAP' */
#define ZD_ZAPP_FLAG_TRUSTED      BIT(0)
#define ZD_ZAPP_FLAG_SINGLETON    BIT(1)
#define ZD_ZAPP_FLAG_WANTS_THREAD BIT(2)   /* honoured post-MVP; see 4.4 */

struct zd_zapp_manifest {
    uint32_t    magic;
    uint16_t    abi_major, abi_minor;
    uint32_t    flags;
    const char *name;
    const char *icon;                       /* NULL in MVP */
    int  (*init) (zd_zapp_ctx_t, const struct zd_host_api *);
    void (*event)(zd_zapp_ctx_t, const struct zd_event *);
    void (*fini) (zd_zapp_ctx_t);
};
```

Desktop side: `EXPORT_GROUP_SYMBOL(DESKTOP, zd_get_host_api);` with
`CONFIG_LLEXT_EXPORT_SYMBOL_GROUP_DESKTOP=y` in the desktop's own Kconfig.
Zapp side: `LL_EXTENSION_SYMBOL(zd_zapp_manifest);`.

**Versioning rule, written into `docs/abi.md` on day one:** `abi_major` must match
exactly; `zapp.abi_minor <= host.abi_minor` is accepted; the vtable only ever grows by
appending, and `struct_size` lets an older zapp safely bind against a newer host. Anything
else is a major bump.

### 4.3 Handle safety

`zd_window_t` is not a raw `struct zd_client *`. It is a pointer into a WM-owned registry
entry `{ uint32_t generation; struct zd_client *client; }`. Every host-API call runs
`zd_handle_deref()`, which validates that the entry is live, that the generation matches,
and **that the client's `owner` is the calling `ctx`'s instance**. A stale or forged handle
gets `-EINVAL`, not a fault, and zapp A cannot manipulate zapp B's windows. This is cheap,
and it is the only part of the isolation story that actually works without an MMU.

### 4.4 Lifecycle

```
discover  scan /system/zapps and <home>/zapps for *.llext  → launcher entries
   ↓ (user clicks)
load      struct llext_fs_loader l = LLEXT_FS_LOADER(path);
          llext_load(&l.loader, name, &ext, &param)
   ↓
bringup   llext_bringup(ext)                          /* .init_array */
   ↓
bind      llext_find_sym(&ext->exp_tab, "zd_zapp_manifest")
          validate magic, abi_major ==, abi_minor <=
   ↓
instance  alloc zd_zapp_instance { ext, manifest, session, windows, id }
          alloc zd_zapp_ctx bound to it
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
thread-per-zapp is precisely where the ABI gets hard (locking discipline, priorities,
per-thread teardown). But the ABI does not preclude it: `ZD_ZAPP_FLAG_WANTS_THREAD` is
defined now and documented as "reserved — the desktop will use `llext_bootstrap()` with a
per-instance stack." The terminal will need it; hello world will not; the contract
doesn't change when it arrives.

### 4.5 Surviving a misbehaving zapp — stated honestly

On this target, without `CONFIG_USERSPACE`, **a loaded llext is trusted code sharing the
kernel address space.** A wild pointer takes down the system. What the MVP actually
provides, and what it does not:

*Genuinely enforced:*
- Handle validation with generation counters + owner checks (§4.3) — no crash from stale
  or cross-zapp handles.
- Per-instance window quota (`ZD_MAX_WINDOWS_PER_ZAPP`) and a global client slab cap.
- Path scoping in the fs shim (§5) — every path normalized, `..` rejected, checked
  against the session's roots.
- ABI version gate at load; a mismatched zapp is rejected before `init` runs.
- Bounded teardown: an instance that fails `init` is unloaded immediately; a zapp whose
  windows are all closed is reaped.

*Not enforced, and the plan says so out loud:*
- Memory safety. No MMU/MPU isolation in the MVP.
- A zapp calling Zephyr APIs directly, bypassing the shim entirely. **Mitigation available
  now and worth taking:** set `CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n` and hand-export only
  what zapps legitimately need. That does not make the shim a security boundary, but it
  makes it the only *linkable* route, which is the difference between a contract and a
  suggestion. (Expect to re-add a handful of libc symbols; budget a task for it.)
- Infinite loops / blocking in a zapp callback. The desktop thread hangs. A watchdog that
  can actually kill a zapp requires the zapp to have its own thread — deferred with
  `ZD_ZAPP_FLAG_WANTS_THREAD`.

*Future hardening path, already reachable on both targets:* `CONFIG_USERSPACE` +
`llext_add_domain(ext, &domain)`, with `ARCH_HAS_USERSPACE` available on arm64 via
`ARM_MMU` and on the RT1060's Cortex-M7 via `ARM_MPU`. Deferred, but not designed out —
this is why every host-API call already carries a `zd_zapp_ctx_t`: those calls become
syscalls without changing a single zapp.

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
host-API entry takes `zd_zapp_ctx_t`; the ctx points at its instance, the instance points
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
/system/zapps/       *.llext, system-installed        app-visible: read-only
/system/share/       fonts, wallpaper, desktop assets app-visible: read-only
/home/user/          the session's home               read-write
/home/user/zapps/    user-installed *.llext           read-write, also enumerated
/tmp/                scratch                          read-write
```

QEMU: FAT over `zephyr,ram-disk` (`disk-name = "RAM"`, 512 B × 4096 = 2 MB),
`FS_MOUNT_FLAG_USE_DISK_ACCESS`, `CONFIG_FS_FATFS_MKFS` to format on first boot.
RT1060: FAT over `zephyr,sdmmc-disk` on `usdhc1`, same paths.

**Getting `.llext` files onto the QEMU FS:** `add_llext_target` emits `hello.llext` into
the build dir; `generate_inc_file_for_target()` embeds it; `zd_seed_install()` writes it
to `/system/zapps/hello.llext` at boot if absent. The discover→open→`llext_fs_loader` path
is 100% real — only the delivery is synthetic, and it mirrors what an installer does. When
zapps move out-of-tree (EDK), swap the seed for the fw_cfg channel (§1.6) and the desktop
image stops needing a rebuild per zapp.

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
    zapp_abi.h                    # THE contract; no Zephyr, no LVGL includes
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
      loader/   zapp_loader.c zapp_instance.c seed.c
  zapps/                         # desktop apps. NOT apps/ -- see note below
    hello/hello.c                # one file — LLEXT_TYPE_ELF_OBJECT allows only one
    notes/notes.c                # second app: multi-window, path_resolve
    badabi/badabi.c              # declares a bad ABI major; must be refused
```

Notes:

- **`west.yml` in the project root, T2 star topology**, `import`ing upstream Zephyr's
  manifest at a pinned `main` commit ≥ 2026-06-15 (`e201b84b04e4` verified). Workspace is
  a *fresh* `west init -l`, separate from `really-native-sim/` — that tree carries your
  portability branches and must not be entangled.
- **Desktop zapps live in `zapps/`, not `apps/`.** `app/` is Zephyr's own convention
  for the application source directory and this project has one, so a sibling `apps/`
  reads as a typo for it every time. The `z` prefix costs nothing and removes the
  ambiguity permanently.
- Zapps are built by the desktop's CMake via `add_llext_target` + `llext_include_directories(... ${CMAKE_CURRENT_SOURCE_DIR}/../include)`,
  but they are **separate ELF artifacts** from `zephyr.elf` from the first commit. One
  `west build` produces `zephyr.elf` and `hello.llext`. The migration to genuinely
  out-of-tree zapp builds is `west build -t llext-edk` + an independent CMake project;
  keeping `include/zd/` outside `app/` is what makes that a move, not a rewrite.
- One source file per zapp is a hard constraint under `LLEXT_TYPE_ELF_OBJECT` (ARM/ARM64
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
  7 of 8 in use` at boot. Raised to 16 in the board conf, before the llext heap and zapp
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
milestone F. It exists to validate handles crossing the zapp ABI, and there are no zapp
handles until F; building it here would have been speculative. The deferred-reap half of
task 12, which is the part with real risk, landed at C and is verified here.

**[D]** A full N-cycle create/close leak assertion still wants an external trigger, so it
lands at F task 24 where the launcher can spawn on demand. The single-cycle slab
accounting is verified.

### E — Filesystem and discovery — **DONE**
13. ✅ 2 MB `zephyr,ram-disk`, FAT, auto-format on first mount, full directory layout.
14. ✅ `host/session.c` + `host/storage.c`.
15. ✅ `host/fs_shim.c`.
16. ✅ `loader/zapp_loader.c` discovery half.
17. ✅ `shell/launcher.c`: Start menu listing discovered zapps; picking one opens a window
    named after it. Verified — the menu shows `hello` and `notes`, both read from the
    filesystem, and the rescan happens on every open so dropping a file in changes what
    the menu shows.

**[E] Paths are not free-form.** FATFS requires a mount point of the form `/<VOLUME>:`,
with the volume string generated from the devicetree `disk-name`, so the root is `/RAM:`
under QEMU and would be an SD volume on hardware. §5's `/system/zapps` and `/home/user`
are therefore *relative to* `CONFIG_ZD_FS_ROOT`, not absolute. This costs nothing because
zapps never build paths themselves — they resolve directories through the session — which
is precisely the indirection §5 already called for, now load-bearing rather than
decorative.

**[E] FAT needs long filenames.** Without `CONFIG_FS_FATFS_LFN`, FAT is limited to 8.3,
which caps extensions at three characters — and every zapp binary ends in `.llext`. Set
alongside `CONFIG_FS_FATFS_MAX_LFN=64`.

**[E]** The two `.llext` files present at this milestone are placeholders written at boot,
not valid ELF, and are never loaded. What is real is the path: opendir, readdir, suffix
match, menu. Milestone F replaces them with a genuine build artifact.

### F — llext load/unload of hello world — **DONE**
18. ✅ `include/zd/zapp_abi.h` complete: vtable, manifest, events, version rule.
19. ✅ `host/host_api.c` with `EXPORT_GROUP_SYMBOL(DESKTOP, zd_get_host_api)`.
20. ✅ `wm/handle.c` — generation-counted registry with owner checks.
21. ✅ `zapps/hello/hello.c`, built by `add_llext_target` into a 2968-byte aarch64
    relocatable ELF.
22. ✅ `loader/seed.c` installs it to `/system/zapps` on first boot.
23. ✅ `loader/zapp_instance.c` — the full lifecycle.
24. ✅ **Verified: 20 consecutive launch/close cycles, 20 loads, 20 unloads, zero errors,
    and after every single one `0 windows live, 8 slab blocks free, 0 zapps live, 0
    handles live`.** The twentieth load succeeding is itself the llext-heap assertion:
    a leak of one instance per cycle would have exhausted the 128 KB heap well before.

**[F] Two host-API tables, not one with a flag.** Trusted zapps get a vtable whose
`unsafe_lvgl_content` slot is populated; everyone else gets one where it is NULL. An
untrusted zapp therefore has no function to call, rather than a function that checks and
refuses.

**[F] Creation and focus had to be separated.** `zd_wm_window_create()` originally focused
the new window itself, which fired the owning zapp's `ZD_EV_WINDOW_FOCUS` before the caller
had attached a handle — so the event was dispatched against handle 0 and silently dropped.
The zapp's title never changed and nothing errored. Creation now stacks but does not focus;
the caller attaches ownership and handle, then focuses. Caught only because hello
deliberately renames its titlebar on focus.

**[F] The zapp imports nothing.** `init()` is handed the vtable, so an extension needs no
symbol from the desktop at all. hello calls `zd_get_host_api()` anyway, purely to force
the loader to resolve a symbol from the export table — otherwise a broken
`EXPORT_GROUP_SYMBOL` would go unnoticed until the first zapp that genuinely needed it.

**[F] `generate_inc_file_for_target`, not `..._for_gen_target`.** The latter makes `app`
depend on the `.llext` but not on the generated `.inc`, so `seed.c` races the generator.

### G — Two instances, and hardening — **DONE**
25. ✅ `zapps/notes` as the second zapp; two live instances of `hello` with cascade
    placement; quota enforced at exactly 4 windows per instance.
26. ✅ `CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n`. **172 exported symbols → 1.** The risk
    flagged in §9 as "may cascade" cost nothing, precisely because the ABI is a vtable:
    zapps import nothing but `zd_get_host_api`. `docs/abi.md` records the measurement.
27. ✅ Negative tests. `zapps/badabi` declares `ZD_ABI_MAJOR + 1` and is installed
    alongside the working zapps, so the version gate is exercised in the field rather
    than in a test directory. `zd_selftest_run()` asserts the rest at boot: 15 checks
    covering traversal, relative paths, out-of-root access, writes to the read-only
    system root, component-boundary prefix matching, and five handle-registry
    properties including that a stale handle does not resolve into the slot's new
    occupant.

**[G] Instances of one zapp share the image.** `llext_load()` refcounts by name, so
launching a zapp twice loads the ELF once and both instances share its `.data` and
`.bss`. A file-scope variable in a zapp is per-*zapp*, not per-instance. ABI 0.2 adds
`set_user_data`/`get_user_data` for exactly this. Three loader consequences, all easy to
get silently wrong: a positive `llext_load()` return means "already resident", not an
error; `.init_array` runs once per image; `.fini_array` must run only for the last
instance holding it.

**[G] No events during `init()`.** Creating a window focuses it, which delivered
`ZD_EV_WINDOW_FOCUS` before the zapp had recorded the handle it was mid-way through
receiving — so the zapp could not recognise its own window. Now a documented ordering
guarantee: events are suppressed during `init` and focus is re-asserted afterwards.

**[G] Log drops can eat the evidence.** The boot burst overflowed the deferred log buffer
and reported `--- 13 messages dropped ---`, silently swallowing the first three selftest
results while the summary still said "all passed". `CONFIG_LOG_MODE_IMMEDIATE=y`. A
self-test whose FAIL line can be dropped is worse than none.

**[G] `add_llext_target` does not rebuild on source change** (Zephyr `main` @ `e201b84b`).
The packaging step depends on a phony target with no file-level dependency on the object,
so the `.obj` recompiles and the `.llext` stays stale — a zapp edit ships the *previous*
binary silently. This invalidated one verification run before it was caught.
`app/CMakeLists.txt` re-attaches the dependency to the documented `pkg_input` property.

### H — Hardware checkpoint, headless — **BUILDS; on-board run pending the EVK**
28. ✅ `hal_nxp` added to the manifest; `app/boards/mimxrt1060_evk_mimxrt1062_qspi.{conf,overlay}`
    with a dummy display at 480×272, FAT on SD (`/SD:`), no seeding, no auto-format.
    **Builds clean: FLASH 324 KB, RAM 284 KB.** All three extensions build as
    32-bit ARM EABI5 relocatables (vs aarch64 for QEMU), `LLEXT_TYPE_ELF_OBJECT`,
    `ARCH_HAS_USERSPACE=y`, and the export surface is 1 symbol here too.
29. ⏸ Requires the board. `docs/hardware.md` is the runbook.
30. ⏸ Requires the board.

**[H] A Kconfig `=n` in a board fragment can be silently overridden.** The board conf set
`CONFIG_FS_FATFS_MKFS=n` to stop a failed mount from formatting the user's SD card, and
the resolved `.config` still read `y`: `FS_FATFS_MOUNT_MKFS` defaults to `y` and
**selects** it. The comment claimed a protection that did not exist. The real control is
the `FS_MOUNT_FLAG_NO_FORMAT` mount flag — the only thing `fat_fs.c:471` actually checks —
now driven by `CONFIG_ZD_FS_AUTOFORMAT`. Worth generalising: after setting a Kconfig
symbol that matters, read it back out of the resolved `.config`.

**[H] Second architecture: M5Stack CoreS3** (`m5stack_cores3/esp32s3/procpu`),
added after the MVP. Xtensa forced the one genuine design change of the whole
port: zapps cannot be streamed off the card, because writable llext storage is
mandatory there and `llext_fs_loader` has no `peek()`. `ZD_ZAPP_LOAD_VIA_BUFFER`
reads the image into RAM and uses `llext_buf_loader` instead. That the change is
confined to `open_loader()` in `zapp_instance.c` -- with discovery, the ABI, the
WM and the shim untouched -- is the strongest evidence so far that the layering
holds. Details in `docs/hardware.md`.

**[H]** Seeding built-in zapps is now `CONFIG_ZD_SEED_BUILTIN_ZAPPS`, off on *this* board.
The point of the checkpoint is that a `.llext` arrives from outside, so the desktop must
find zapps it did not write itself. Not a rule for hardware in general, and the CoreS3
later took the other answer deliberately: seeding is itself a write to a card sharing a pin
with the display, so it exercises the arbiter every boot, and a plain FAT card then boots
to a populated desktop. External delivery stays checkable there by deleting a zapp and
copying your own in. The Kconfig help states both.

### I — Storage in the zapp ABI — **DONE (ABI 0.3)**

Post-MVP. The filesystem half of §4.2 was specified and never built: the vtable shipped
`path_resolve()` and nothing that could act on the answer, so `host/fs_shim.c` — a file
whose header describes it as the choke point every zapp filesystem call passes through —
was reached only by the boot selftest.

31. ✅ ABI 0.3: `zd_file_t`, `zd_dir_t`, `ZD_O_*`, `struct zd_dirent`, and fourteen
    appended vtable slots (open/read/write/seek/tell/sync/close, opendir/readdir/closedir,
    stat/mkdir/unlink/rename). Purely additive; `struct_size` and the minor gate did the
    rest.
32. ✅ `host/fs_api.c`, the only place zapp storage touches Zephyr's `fs_*`. Resolve
    through the shim, bracket with the bus arbiter, hand back a generation-counted
    handle. Fixed arrays for open objects; a per-instance quota covering files and
    directories together.
33. ✅ `ZD_EV_CLICK` is now actually delivered — it had been a defined enum value nothing
    ever sent, which made "a zapp reacts to the user" impossible. `wm->on_client_click`,
    fired from the content area only, in content-relative coordinates.
34. ✅ `zapps/notes` earns its name: it appends a line per click to `$HOME/notes.txt` and
    shows the file back on relaunch, from every window it owns.
35. ✅ Fourteen new boot selftests. Verified on all three targets; the CoreS3 run is the
    real one, since it writes an SD card through the GPIO35 arbiter with the display live.
    Confirmed by touch on that board: tapping Notes appends to
    `/SD:/home/user/notes.txt` and the window redraws from the file, so what is on screen
    is what reached the card.

**[I] The handle registry generalised for free.** Files and directories needed one new
`enum zd_handle_kind` value and nothing else — same slots, same generation counter, same
owner check. The scheme was written for windows and turns out not to have been about
windows.

**[I] `zd_fs_close_all()` closed files but leaked their handles.** Harmless in the real
path, where instance teardown calls `zd_handle_free_all()` immediately after — and
therefore invisible, until the boot selftest (which has no teardown) reported a baseline
of four live handles that should have been zero. The slot now remembers its own handle so
the operation is complete on its own. An "it works because the caller happens to clean up
after us" is a bug with a delay fuse.

**[I] A zapp has no libc, and the compiler does not know that.**
`CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n` means one importable symbol, so `strlen` and
`snprintf` are unavailable — which is easy to remember. What is not: GCC *synthesises* a
call to `memset` from an ordinary struct assignment. `notes.llext` built cleanly and would
have failed at load with an undefined symbol a long way from the line responsible. Caught
by `nm -u` on the artifact, which is now the thing to check after touching a zapp.

**[I] Reads are contractually short.** `CONFIG_ZD_FS_IO_CHUNK` caps a single transfer.
With zapps on the desktop thread, an unbounded read stops the UI — and on the CoreS3 it
holds GPIO35 away from the display, so the screen cannot be drawn at all while it runs.
Bounding the call is the honest mitigation for staying single-threaded; making I/O
asynchronous means thread-per-zapp, which is still reserved.

### J — Window management, round 2 — **DONE (ABI 0.4)**

Post-MVP. The WM is one of the two things `CLAUDE.md` calls load-bearing and was the
one untouched since milestone D. A window could not be resized, could not be got out of
the way without being destroyed, and once covered could only be reached by clicking a
visible corner of it. `zd_wm_window_set_geometry()` logged *"resize is not implemented,
moving only"*; `ZD_EV_WINDOW_CLOSE_REQUEST` was still an enum value nothing ever sent.

36. ✅ A client can be **unmapped**. `client->minimized` keeps the client in `wm->stack`
    and hides it in the projection, which is the X11 map/unmap distinction and the thing
    `stack.c`'s header comment had been promising since milestone A. `zd_wm_top()` returns
    the topmost *mapped* client; restore is exact rather than approximate, because nothing
    ever left the ordering.
37. ✅ **Resize**, by a grip in the bottom-right corner, through the same handler as
    drag-to-move with `client->drag_mode` telling them apart. The sizing arithmetic came
    out of `zd_client_build()` into one `layout_subtree()` that both build and
    `apply_geom()` call, which is what makes resize three lines instead of a second copy
    of the chrome layout.
38. ✅ **A window list in the taskbar** (`shell/tasklist.c`), rebuilt wholesale from
    `wm->stack` and driven by a new `wm->on_client_list_changed` hook. Without it a
    minimised window is simply gone.
39. ✅ **The close handshake.** The close box now goes through
    `zd_wm_window_close_request()`: ask the zapp, give it `CONFIG_ZD_CLOSE_GRACE_MS`,
    close it anyway when that expires or on a second click. The first thing the desktop
    has ever asked a zapp to *do* rather than merely told it.
40. ✅ ABI 0.4 — `ZD_EV_RESIZED`, `ZD_EV_MINIMIZED`, `ZD_EV_RESTORED`, a `resize` member
    in the event union, and `window_minimize`/`window_restore`. Nineteen new boot
    selftests (51 in the image). Verified on all three targets; `notes` handles all of it.

**[J] Enum values are wire format.** The tidy place for `ZD_EV_RESIZED` is next to the
other window events. Putting it there renumbers `ZD_EV_CLICK` and silently breaks every
0.3 zapp — the vtable's append-only rule applies to the event enum too, and nothing in
the versioning machinery would have caught it. New values go on the end, with a comment
saying why they are not where you would look for them.

**[J] `lv_obj_set_size()` does not resize anything.** It marks the object dirty; the
coordinates are recomputed at the next layout pass. So the obvious implementation of the
resize event — set the size, then read the content area's width back to put in the
payload — hands the zapp *the size the window used to be*, while every other symptom of
the resize looks correct. `zd_client_content_size()` derives it from `client->geom`
instead, which is what "the model is the truth, LVGL is told" meant all along. Two boot
selftests failed on this before it was understood, which is the only reason it was.

**[J] Hit slop does not compose.** `lv_obj_set_ext_click_area()` is right for an isolated
control and wrong for adjacent ones — LVGL awards an overlap to the last-added child, so
the minimise and close buttons, 2 px apart with `CONFIG_ZD_TOUCH_SLOP_PX=12` on the
CoreS3, would have sent *every* tap on minimise to the close box. This is the same bug
that cost the launcher menu its top entry in milestone E, met a second time in a
different disguise. The controls now grow with the slop as real pixels
(`ZD_BTN_SZ`, `ZD_GRIP_SZ`, and `ZD_TITLEBAR_H` with them) and carry no ext area at all;
where the slop is 0 the chrome is byte-identical to before. A boot check asserts the
rectangles do not overlap, on the target, because the value that breaks it lives in a
board fragment.

**[J] A move is not a resize, and the difference is a frame budget.** Factoring the chrome
layout into `layout_subtree()` and having `zd_client_apply_geom()` call it is right for
resize and quietly wrong for drag: a move went from one `lv_obj_set_pos()` to eight LVGL
calls and three layout invalidations, *per pointer sample*. On the CoreS3 the FT5336 polls
every 20 ms, the desktop loop fell behind, and the input queue overflowed —
`<wrn> input: Event dropped, queue full` — so windows would not drag at all. Invisible on
QEMU, where the pointer is cheap and the loop has slack. Split into `zd_client_apply_pos()`
and `zd_client_apply_geom()`. The general shape: a refactor that makes the *expensive* case
correct can make the *common* case unaffordable, and only the slowest target says so.

**[J] The seed compared file sizes, not file contents.** `install_one()` skipped a zapp
already on the volume if `entry.size` matched, so a rebuilt `notes.llext` that happened to
land on the same 5096 bytes was never replaced — the board ran the previous build's
extension against the new host and said so once, in
`launched 'Notes' instance 4 (ABI 0.3)`, in a log line nobody was reading. Two rounds of
hand-testing measured the wrong binary. It now reads the installed copy back and compares
it, and logs at `WRN` when it replaces something. Equal length is not equal content, and a
staleness check that can be wrong silently is worse than none.

**[J] The deferred-destroy rule has a second customer.** Clicking a taskbar button changes
focus, which fires `on_client_list_changed`, which would `lv_obj_clean()` the row holding
the button whose `LV_EVENT_CLICKED` dispatch is on the stack. Exactly the hazard
`CLAUDE.md` describes for zapps closing their own windows, arrived at from the shell side
by a route with no zapp in it. `zd_tasklist_invalidate()` sets a flag and
`zd_tasklist_reap()` runs from the desktop loop, after `zd_wm_reap()` so it never rebuilds
from a stack still holding windows that are about to go.

---

### K — Notepad, and the OS underneath it — **DONE (ABI 0.5)**

Post-MVP, and the first milestone driven by an application rather than by the
spine. `hello`, `notes` and `badabi` are instruments; the ask was for something a
person would use, modelled on Windows Notepad, "developing needed OS features
along the way, including but not limited to copy and paste". Almost all of the
milestone is those features.

41. ✅ **A zapp can be more than one file.** ARM and ARM64 moved to
    `LLEXT_TYPE_ELF_RELOCATABLE` in their board fragments; Xtensa already
    defaulted to `ELF_SHAREDLIB` and was left alone. Risk #6 in §9 named the text
    editor as the thing that would hit the one-file wall and it did — Notepad is
    four files. `zapps/lib/zapplib.c` is the immediate payoff: the no-libc
    helpers written once instead of once per zapp. `hello` deliberately stays a
    single translation unit, so the simplest case stays proven.
42. ✅ **`CONFIG_ZD_SMOKE_TEST`**, which made everything after it checkable. The
    desktop was only testable with a pointer; a headless run said the image
    booted and nothing about whether an extension still loaded. It discovers,
    launches everything, closes every window through the polite path, waits out
    the grace period, and compares client, handle, open-file and instance counts
    against boot.
43. ✅ **A key path**, `input/keys.c`, with two sources that cannot tell each
    other apart: `input/keymap.c` over Zephyr's input subsystem (a real virtio
    keyboard on QEMU, added by one `-device` and one devicetree node) and
    `shell/osk.c`, an on-screen keyboard. `wm/keys.c` is the whole routing
    policy and is four lines long. `ZD_EV_KEY` is delivered at last.
44. ✅ **An overlay layer.** `struct zd_layers` grew a fourth member, above the
    taskbar, for the things that are not windows: the Start menu (which loses its
    reparent-onto-the-screen trick), menu drop-downs, the on-screen keyboard, and
    dialogs with the click-swallowing shade that makes them modal by
    construction.
45. ✅ **A text widget** (`zd_text_t` over `lv_textarea`), a **clipboard**
    (`host/clipboard.c`, the first genuine desktop *service*), a **menu bar** in
    the chrome, **modal dialogs** with a file picker, and a **clock** service the
    taskbar and Notepad share.
46. ✅ **Notepad.** File and Edit menus, accelerators, a dirty marker in the
    title, open and save through the picker, and a close handshake that argues
    with you. Zero undefined symbols in its `.llext`.
47. ✅ ABI 0.5, all appends: `ZD_KEY_*`, `ZD_MOD_*`, `ZD_TEXT_*`, `ZD_DLG_*`,
    `ZD_EV_TEXT_CHANGED`/`_MENU`/`_DIALOG`, a widened `ev->key`, ~35 vtable
    slots, and `ZD_TITLE_MAX` moved out of the WM into the contract.
    102 boot selftests, up from 51.

**[K] The key path touched LVGL from the input thread.** Two keystrokes into the
first Notepad test: `ZEPHYR FATAL ERROR 2: Stack overflow on CPU 0, Current
thread: input`. The stack was the symptom — that thread is small and
`LOG_MODE_IMMEDIATE` formats on the caller's stack — and the locking was the
fault. Everything downstream of routing a key is LVGL's, and only the desktop
loop may touch that. Keys are now queued and drained from the loop, which is
exactly what LVGL's own pointer driver does; the pointer got it for free from
Zephyr's glue, so nobody had to think about it, and keys are ours. **Any new
input modality inherits this, not the shape of the first attempt.**

**[K] Deferring destruction is not enough; opening must be deferred too.** The
answer to one dialog is very often another dialog — Notepad's close handshake is
"save it?" → Yes → "save as what?". Building the second inline would
`lv_obj_clean()` the panel holding the Yes button whose dispatch was still on
the stack: the same use-after-free `CLAUDE.md` warns about, arrived at from the
opposite direction. Both ends of a dialog now go through the loop. The rule
generalises: *any* surface rebuilt in response to an event needs the treatment,
not just surfaces destroyed by one.

**[K] "An ask, not a veto" was too strong, and made Cancel decoration.** 0.4 gave
a zapp two options — close, or be closed when the grace period expired — which
meant declining and being wedged were indistinguishable, and a *save changes?*
box lost its window two seconds later whatever the user clicked. Notepad was the
first zapp with a real answer and the flaw was immediately obvious.
`window_close_cancel()` is now the third option. The grace period is aimed at
**silence**, not at refusal; the desktop also stops counting down while a dialog
that zapp asked for is on screen, because asking the user is doing what was
requested. The cost — a zapp determined to decline forever keeps its window — is
the bargain every desktop makes, and every desktop answers it with an end-task.
This one does not have one yet, and that is now on the deferred list.

**[K] The selection range was reachable through public API after all.**
`lv_textarea` exposes only "is anything selected", which looks like a dead end
and nearly cost this project its first `#include` of an LVGL private header. The
range lives on the label underneath, where `lv_label_get_text_selection_start()`
and `_end()` are public. Worth the ten minutes it took to look: no LVGL private
header is included anywhere in this tree, and it stays that way.

**[K] `list(APPEND QEMU_EXTRA_FLAGS)` in the application's CMakeLists never
arrives, and `CONFIG_QEMU_EXTRA_FLAGS` kills the build.** `cmake/emu/qemu.cmake`
is included from `zephyr/CMakeLists.txt` during `find_package(Zephyr)`, so the
run target is fully assembled before the application's own body is read — the
append has to happen *above* `find_package`. The Kconfig route reaches
`qemu.cmake` correctly and then dies in a post-link script:
`scripts/build/llext_inspect_discarded_groups.py` parses `.config` with
`line.split("=")` and no maxsplit, so any value containing a second `=` — such
as `bus=virtio-mmio-bus.4` — raises *"ValueError: too many values to unpack"*.
Upstream bug at Zephyr main `e201b84b`.

**[K] The llext heap is priced in regions, not bytes.** 128 KB held three small
zapps and stopped holding four the moment Notepad existed, despite the four ELFs
totalling about 30 KB. llext rounds every region of every extension up to a 4 KB
alignment, so the cost tracks region count far more than image size. The failure
is a `-ENOMEM` out of `llext_load()` that looks exactly like a corrupt
extension — and here it quietly replaced `badabi`'s ABI-major refusal with an
out-of-memory one, so the version gate stopped being tested and the smoke test
still said "1 refused". 256 KB now.

**[K] A zapp reaching for the desktop's Kconfig compiles.** Zapps are built with
`-imacros autoconf.h`, so `CONFIG_ZD_TEXT_MAX` and `CONFIG_ZD_MAX_CLIENTS` are
right there and work. Using them welds the zapp to one desktop build and breaks
the out-of-tree story `include/zd/` exists to protect. Notepad asks instead —
`text_get_capacity()` was added for exactly this — and keeps its own
`NP_MAX_INSTANCES`.

### L — A file browser, and the OS underneath it — **DONE (ABI 0.6)**

§9 named the file browser and the terminal as the next two zapps and said the
browser was the cheaper one. It was, but not for the reason given there.

The expectation was that a browser would need storage. It needed none:
`fs_opendir`, `fs_readdir`, `fs_stat`, `fs_mkdir` and `fs_unlink` have been
zapp-facing vtable slots since **0.3** and no zapp had ever called one of them —
storage arrived for Notepad, which only ever opened a path somebody else had
chosen. `struct zd_dirent` already carried everything Zephyr's `struct fs_dirent`
has. What was missing was a way to *show* a directory: a zapp never sees an
`lv_obj_t`, and the only content slots were a label and a text field.

51. ✅ **`ZD_PATH_MAX` 96 → 192**, first, so nothing later needed re-auditing.
    Safe because the constant is in no struct in the header and every host call
    that writes a path into a caller's buffer is told its size — so raising it
    moves no offset. `ZD_NAME_MAX` is the counter-example and now says so: it
    *is* an array bound inside `struct zd_dirent`, `fs_readdir` has no length
    parameter, and raising it would smash a correctly-versioned 0.5 zapp's stack
    with nothing in the version gate able to notice. Frozen.
52. ✅ **`chrome/rowlist.c`**, a scrolling column of selectable rows, shared by
    the ABI widget and the file picker. Model and view are separate; see the
    finding below.
53. ✅ **`zd_list_t` in the ABI**, over the rowlist, exactly as `text_api.c`
    layers over `lv_textarea`. Plus `label_set_pos()` and `label_destroy()`,
    because 0.5 could create a label and then neither move nor free it — which
    survived only because no label's position depended on anything, and a status
    line along the bottom of a resizable window is the first that does.
54. ✅ **The file picker rebuilt on it, with the navigation §8 listed as
    missing.** Folders show, `..` comes back up, and the floor is the directory
    the caller named rather than the volume root.
55. ✅ **`dialog_prompt()`**, the third dialog kind. Confirm answers a question
    the desktop asked, the picker answers "which of these"; neither answers
    "what shall it be called".
56. ✅ **`zapp_launch(name, arg)` and `get_launch_arg()`.** The second desktop
    *service* after the clipboard, and what makes double-clicking a document
    mean anything. Deferred through the loop rather than run inline like the
    Start menu's launch, because the caller is another zapp.
57. ✅ **`ZD_ZAPP_FLAG_SINGLETON` enforced**, having been defined and ignored
    since 0.1.
58. ✅ **`zapps/files/`**, the browser. Navigates in place, folders first, a
    status line, New Folder and Delete, and a double-clicked `.txt` handed to
    Notepad.
59. ✅ **Boot checks 102 → 160**, and two bugs found on the way: see below.

**[L] The model/view split is what makes a rebuildable widget safe.** Entering a
directory means emptying and refilling the list from inside the dispatch of a row
in that list — CLAUDE.md's central hazard, met a fifth time. Rather than another
ad-hoc deferral, the list keeps a model that the zapp mutates and a view the loop
re-derives. Getters answer from the model immediately, so nothing has to reason
about when the pixels catch up. This is not new law: it is what the WM already
does with `sys_dlist_t` and z-order. Two things fell out of it that the safety
argument alone would not have bought — the picker's habit of keeping filenames
alive by reading them back off LVGL labels is gone, and the bus arbiter no longer
wraps `lv_obj_create()`, because filling the model and building rows ended up in
different functions on their own.

**[L] LVGL sends `DOUBLE_CLICKED` before `CLICKED`, to the same object.** So a
double-click on a directory row runs activate, the zapp rebuilds the model, and
*then* a click arrives at the same still-alive row carrying an index into a
listing that no longer exists. Two guards: clearing hides the container at once
so no new press can reach a stale row, and every row callback re-validates
against the dirty flag first. Worth knowing before writing the code rather than
after — and the reason to use LVGL's streak detection at all is that its
thresholds are better than a hand-rolled timer's: the movement tolerance is the
scroll limit, so a finger that travelled far enough to scroll the list was
scrolling it.

**[L] Every menu bar since K has been spaced by an LVGL default.**
`zd_menu_add_submenu()` placed each title after the previous one by summing
`lv_obj_get_width()` — read immediately after `lv_obj_set_size()` and therefore
before any layout pass, so it answered 130 px instead of the ~33 px a title
actually is. CLAUDE.md has stated that rule since K; this was it being broken
three lines after it was written down. Visible as a wide dead gap between File
and Edit, which reads as slightly odd rather than as a bug — and invisible and
far worse on a window narrower than about 260 px, where the second title lands
past the end of the bar and cannot be clicked at all. Notepad's window happened
to be wide enough; the browser's is not, and hunting for its unreachable Go menu
is what turned this up. The boot check that should have caught it asserted only
that adjacent titles do not *overlap*, and two titles a hundred pixels apart do
not overlap. It now asserts they sit edge to edge.

**[L] `app/smoke.conf` never existed.** CLAUDE.md has documented
`-DEXTRA_CONF_FILE=smoke.conf` since K and the file was never committed, so the
command always failed at CMake time — and a failed configure leaves the previous
build directory intact, so running the smoke test afterwards executes the last
good image and reports a pass. The same stale-binary trap as K's filtered build
log, reached from a new direction: not a hidden error this time, but a build that
never ran.

**[L] Nothing on the overlay was asking how much screen the keyboard had taken.**
Found on the CoreS3 after the milestone was otherwise done. A Save As box came
out 249 px tall on a 240 px display, put its filename field behind the keys and
its buttons over the taskbar's keyboard toggle -- and, because a dialog raises
its shade while `zd_osk_set_visible(true)` does nothing when the keyboard is
*already* up, left the keyboard below that shade: visible and completely inert.
Three faults in one screen, none of them reachable on QEMU, where the slop is 0
and the keyboard is never raised automatically. The fix is a rule rather than a
patch -- `zd_osk_height()` exists so anything on the overlay can ask, dialogs
size themselves against what is left and are rebuilt when it changes, and the
modal layering is stated (shade, keyboard, panel) instead of being whatever
moved last. See `docs/hardware.md`.

**[L] A zapp had never been refused by the permission shim.** Notepad only wrote
where a picker had already sent it. New Folder in `/system/zapps` is the first
time a zapp has asked for something it could not have, and it correctly gets
`-EACCES` and says "This folder is read-only" — raising that complaint from
inside another dialog's answer, which is the chained-dialog path K built and
nothing had used.

---

## 8. Explicitly deferred

Named so they don't leak into the MVP: any *further* catalog zapp (image
viewer, media player, terminal, web server, web browser); hardware acceleration (PXP
exists and is one Kconfig away — not now); MCU→MPU responsive layout morph; **maximise**,
window snapping, and resize from any edge but the bottom-right corner; notifications;
multiple displays; hardware-enforced isolation (`USERSPACE` + `llext_add_domain` + memory
domains); a theming engine (MVP hardcodes one palette); sound; real multi-user login;
thread-per-zapp (`ZD_ZAPP_FLAG_WANTS_THREAD` is reserved, not honoured); out-of-tree zapp
builds via the llext EDK; the fw_cfg zapp-delivery channel; zapp icons; `native_sim`.

New to this list after milestone K, and each of them named by something that was
actually wanted rather than imagined:

- **An end-task.** `window_close_cancel()` lets a zapp decline a close, which is
  correct and is what makes a *save changes?* box mean anything — and it means a
  zapp that declines forever keeps its window. Every desktop answers that with a
  force-quit out of band; the taskbar's window list is the obvious home for one.
- **Undo.** `lv_textarea` has no undo stack, and one that survives cut, paste and
  select-all is its own milestone rather than a corner of Notepad's.
- **Find and Replace, word wrap toggle, print.** Each needs a dialog or a service
  that does not exist; a greyed-out Search menu would misrepresent how finished
  this is.
- **A real clock, and a date.** `clock_now()` reports the desktop's fiction --
  the call is the right shape, only its implementation is a lie, and only on
  boards with no RTC -- and there is no date in the ABI at all. Together those
  are why **Notepad's Time/Date went in for fidelity and came straight back
  out**: it stamped half of what the original stamped, and stamped a made-up
  time into a document the user then saves. A made-up time in a corner of the
  taskbar is a nicety; the same number written into a file is a small lie with a
  long life. Worth having when both halves are real.
- **A keyboard layout that is not US.** `input/keymap.c` says so at the top.

New to this list after milestone L:

- **File associations.** `zapp_launch()` takes a zapp name, and the browser
  hardcodes `.txt` → `notepad` and says so in its own source. A registry is the
  honest version, and it wants somewhere to live -- a file under `/system` that
  the desktop reads at boot -- rather than a table compiled into one zapp.
- **Rename.** `fs_rename` exists and the prompt dialog exists, so the dialog
  version is a few lines. The version worth having renames in place, in the row,
  which is its own piece of work.
- **Noticing that somebody else changed a directory.** Nothing notifies anyone
  of anything in this desktop. The browser refreshes when you navigate; if
  Notepad saves a file into the folder you are looking at, you will not see it
  until you leave and come back. A change-notification service is the general
  answer and the taskbar would want it too.
- **Sorting, and any column but the name.** Folders come first and then
  whatever order the filesystem gave. There is one proportional font and no
  table widget, and "Modified" is impossible for a third separate reason --
  `struct zd_dirent` has no timestamp, Zephyr's `struct fs_dirent` has none to
  give, and `clock_now()` still has no date in it.
- **Multi-select.** `list_create()` takes a flags word with nothing defined in
  it, which is where that would go.

**Filesystem access from a zapp left this list in ABI 0.3** — see milestone I.
**Window resize, minimise, the taskbar window list and `ZD_EV_WINDOW_CLOSE_REQUEST` left
it in ABI 0.4** — see milestone J.
**Keyboard input, the clipboard, and the text editor left it in ABI 0.5** — see
milestone K.
**The file browser, list widgets, picker navigation and launching one zapp from
another left it in ABI 0.6** — see milestone L.

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
6. ~~**One source file per zapp** under `LLEXT_TYPE_ELF_OBJECT`.~~ **[K] Retired.**
   It was an immediate wall for the text editor, exactly as predicted.
   `LLEXT_TYPE_ELF_RELOCATABLE` works on both ARM targets and cost one line per
   board fragment.

**Where I'd want your input, when we reach it:**

- **Task 26 outcome** — if narrowing the export surface proves painful, is the honest
  "shim is a contract, not a boundary" note sufficient for you, or do you want the
  surface narrow even at the cost of zapp ergonomics?
- **Milestone H, step 30** — whether to buy the `rk043fn66hs_ctg` before or after the
  headless hardware checkpoint. Cheap either way; it just reorders the fun.
- ~~**First post-MVP zapp.**~~ **[K] Answered: the text editor.** It forced keyboard
  input and multi-file extensions as predicted, and four things that were not on
  the list — a text widget, a clipboard, menus and dialogs. The terminal
  (`WANTS_THREAD`) and the file browser (a list widget, and the picker's missing
  navigation) are the next two, and both are now cheaper than they were.
- ~~**Second post-MVP zapp.**~~ **[L] Answered: the file browser.** The list
  widget and the picker's navigation were predicted correctly. What was not is
  that it needed *nothing* from storage — every call it makes had been in the
  ABI unused since 0.3 — and that it would force a way for one zapp to launch
  another, which is the first thing here that makes two zapps cooperate. **The
  terminal is the remaining one**, and it is the expensive one:
  `ZD_ZAPP_FLAG_WANTS_THREAD` is now the only flag still defined and not
  honoured, and honouring it means locking discipline, priorities, and tearing
  down a thread that may be blocked. Nothing about L made it cheaper.

## 10. Verification

- **Per milestone:** `west build -b qemu_cortex_a53 app && west build -t run`, and look at
  the cocoa window. Every milestone A–G is demoable this way.
- **The success criterion, end to end (after G):** boot → retro desktop with patterned
  background, taskbar, live clock → click launcher → menu lists `hello` and `hello2`
  discovered from `/system/zapps` → click `hello` twice → two windows, cascaded, retro
  chrome → drag one over the other by its titlebar, click each to raise → correct z-order
  and titlebar focus styling → close both via the close button → console shows
  `llext_unload` for each, and the instrumented client-slab and llext-heap counters return
  to their pre-spawn values.
- **Leak check** is an explicit assertion in task 24, not an eyeball: capture slab
  in-use count and `llext` heap free bytes at boot, after N spawn/close cycles, and
  compare.
- **Negative tests** (task 27) are run manually against a deliberately broken zapp built
  with a bumped `ZD_ABI_MAJOR` and a hand-corrupted magic.
- **Hardware (H):** console log only — discovery, load, window create, unload — plus
  recorded RAM/flash footprint.
