# Hardware targets

Two boards: the **MIMXRT1060-EVK** (headless, milestone H) and the
**M5Stack CoreS3** (a self-contained touchscreen device, and the project's
second architecture). Jump to [CoreS3](#m5stack-cores3) for the latter.

## MIMXRT1060-EVK

The hardware checkpoint is deliberately **headless**. No panel is attached yet,
so the desktop runs against a dummy display controller: the WM, the loader, the
filesystem and the zapp ABI are all exercised on real Cortex-M7 silicon, and only
the pixels are absent. LVGL still renders into a buffer, so this is a real run
rather than a compile check.

## Why this board is a good bet

| | |
|---|---|
| **llext** | Cortex-M7 is arch `arm` — supported, and `LLEXT_TYPE_ELF_OBJECT` exactly as on the QEMU target |
| **Future hardening** | `ARM_MPU` present ⇒ `ARCH_HAS_USERSPACE`, so `CONFIG_USERSPACE` + `llext_add_domain()` is reachable |
| **Storage** | `zephyr,sdmmc-disk` on `usdhc1`, `disk-name = "SD"` ⇒ FATFS mount point `/SD:` |
| **Display (later)** | eLCDIF (`display_mcux_elcdif.c`) + in-tree shield `rk043fn66hs_ctg`: **480×272**, the same geometry the QEMU target already uses, so the layout needs no re-tuning |
| **2D acceleration (later)** | `CONFIG_LV_USE_PXP` / `CONFIG_LV_USE_GPU_NXP_PXP` exist in the Zephyr LVGL module. The PXP plugs in as an LVGL draw unit — no WM change, which is exactly the property the design preserved by never touching a framebuffer directly |

## What differs from QEMU

Everything is in `app/boards/mimxrt1060_evk_mimxrt1062_qspi.{conf,overlay}`:

- `CONFIG_ZD_FS_ROOT="/SD:"` instead of `/RAM:`. Zapps never notice — they resolve
  directories through the session, which is why that indirection exists.
- SD card instead of a RAM disk.
- **`CONFIG_ZD_SEED_BUILTIN_ZAPPS=n`.** The point of this checkpoint is that a
  `.llext` arrives from *outside*, so the desktop must find zapps it did not write
  itself.
- **`CONFIG_FS_FATFS_MKFS=n`.** Formatting someone's SD card because we failed to
  read it would be a poor first impression; fail the mount and say so.
- Dummy display controller as `zephyr,display`, at 480×272.

No pointer device is configured, so nothing is clickable. The console is the
interface for this milestone.

## Running it

```sh
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1
west build -p -b mimxrt1060_evk/mimxrt1062/qspi app -d build-rt1060
```

Prepare a FAT-formatted card:

```
/system/zapps/hello.llext    <- copy build-rt1060/hello.llext
/system/zapps/notes.llext    <- copy build-rt1060/notes.llext
/system/share/
/home/user/zapps/
/tmp/
```

`hello.llext` **must be the one from the RT1060 build** — it is a Cortex-M7
object, not the aarch64 one QEMU uses. The desktop will refuse the wrong
architecture at relocation, which is the right outcome but an easy half hour to
lose.

Then `west flash -d build-rt1060`, and watch the console.

## What to look for

The desktop has no input device here, so it cannot be driven — the interesting
output is the boot sequence:

```
mounted /SD:
session uid=1000 user='user' home='/SD:/home/user'
selftest: all checks passed
discovered app 'hello' at /SD:/system/zapps/hello.llext
discovered N app(s)
zephyr-desktop up on <display>
```

That alone is the checkpoint: a filesystem the desktop did not create, zapps it
did not install, and the permission and handle checks passing on real silicon.

To prove load/unload without a pointer, temporarily launch from `main()` after
`zd_launcher_init()`:

```c
struct zd_zapp_entry entry[ZD_MAX_DISCOVERED];
int n = zd_zapps_discover(&session, entry, ZD_MAX_DISCOVERED);
if (n > 0) {
        zd_zapp_launch(&entry[0]);
}
```

Expect `llext: Loaded extension hello`, `launched 'Hello' instance 1 (ABI 0.2)`,
and `[Hello] hello world`.

## Numbers to record

- `west build` footprint (FLASH and RAM) — compare against the QEMU figures.
- Whether the SD mount succeeds first try; `power-delay-ms = <1000>` in the board
  dtsi suggests card power-up timing has bitten people before.

## When a panel arrives

Buying an `rk043fn66hs_ctg` upgrades this milestone to the full graphical
desktop:

1. Build with `-DSHIELD=rk043fn66hs_ctg`.
2. Delete the `dummy_dc` node from the overlay; the shield supplies
   `zephyr,display` (eLCDIF) and a GT911 as `zephyr,touch`.
3. `CONFIG_LV_Z_POINTER_FROM_CHOSEN_TOUCH` then wires the touch panel to LVGL
   with no code, exactly as `virtio_input0` does under QEMU.
4. `CONFIG_LV_USE_PXP=y` becomes a one-line acceleration experiment.

Nothing above the board layer should need to change. If it does, that is a bug
in the layering, not in the port.


---

# M5Stack CoreS3

`m5stack_cores3/esp32s3/procpu`. Unlike the RT1060 this is a finished device --
320×240 capacitive touchscreen, microSD, battery, no ribbon cables -- and it is
the only target that is **not ARM**. That is the point of having it: everything
above the loader is arch-agnostic by construction, and this is the board that
tests the claim rather than repeating it.

The board DTS already chooses everything the desktop looks for, so the touch
panel reaches LVGL with no code of ours:

```
zephyr,display = &ili9342c;      /* 320x240 */
zephyr,touch   = &ft6336_touch;  /* focaltech,ft5336 */
sdmmc-disk, disk-name = "SD"     /* -> CONFIG_ZD_FS_ROOT="/SD:" */
```

Note the touch controller does **not** appear in the board's Supported Features
table in the Zephyr docs; it is only visible in the DTS. Don't rule the board
out on that table.

## What Xtensa forced

**Zapps cannot be streamed off the card here.** Xtensa requires
`CONFIG_LLEXT_STORAGE_WRITABLE` (Zephyr issue #75341; upstream's test matrix
excludes xtensa from every `llext.readonly` scenario with the comment "Xtensa
needs writable storage"). Writable storage needs a loader implementing `peek()`,
and `llext_fs_loader` sets `.peek = NULL`.

A second, independent reason points the same way: this SoC sets
`ARCH_HAS_WORD_GRANULAR_ACCESS_INSTR_MEM`, and the docs state that *only*
`llext_buf_loader` may be used when the instruction heap lives in
word-granular-access instruction memory.

So `CONFIG_ZD_ZAPP_LOAD_VIA_BUFFER=y` reads the whole `.llext` into a heap
buffer and hands that to `llext_buf_loader`. The buffer is owned by the instance
and freed in `unwind_image()`, because llext may reference straight into it for
the zapp's whole lifetime.

**Discovery is untouched.** The zapp is still a real file found by scanning a
real filesystem; it just takes one hop through RAM on the way in. Nothing in the
ABI, the WM, the handle registry or the permission shim changes.

## Other Xtensa-specific settings

- **ESP32-S3 is Harvard.** `CONFIG_LLEXT_HEAP_SIZE` does not exist here; the heap
  splits into `LLEXT_INSTR_HEAP_SIZE` and `LLEXT_DATA_HEAP_SIZE`, whose defaults
  (4 KB / 8 KB) are far too small. Kconfig at least *warns* when you set the
  wrong one, unlike the silent `select` override that bit the RT1060 conf.
- `CONFIG_SYS_HEAP_BIG_ONLY=y`, or the IRAM heap faults during init.
- Extensions build as **ELF shared objects** (`LLEXT_TYPE_ELF_SHAREDLIB` is the
  Xtensa default), not the relocatable objects ARM produces.
- DRAM is much tighter than the 512 KB SRAM figure suggests: the first build
  overflowed `dram0_0_seg` by 24 KB. The LVGL pool is 32 KB here rather than 96.
  The 8 MB of PSRAM is the real headroom — enabling `CONFIG_ESP_SPIRAM` and
  placing the LVGL pool there via `LV_Z_MEMORY_POOL_ZEPHYR_REGION` would remove
  the constraint. Not done yet.

## Building and flashing

Requires `esptool` on PATH (`uv tool install esptool`, or
`west packages pip --install`).

```sh
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1
west build -p -b m5stack_cores3/esp32s3/procpu app -d build-cores3
west flash -d build-cores3
```

Builds clean at **513 KB flash**. Prepare a FAT-formatted microSD exactly as for
the RT1060, but copy the **CoreS3** artifacts — `build-cores3/*.llext` are Xtensa
shared objects and the aarch64 or Cortex-M7 ones will be refused at relocation:

```
/system/zapps/hello.llext    <- copy build-cores3/hello.llext
/system/zapps/notes.llext    <- copy build-cores3/notes.llext
/system/share/
/home/user/zapps/
/tmp/
```

## Layout at 320x240

The chrome uses fixed pixel constants and does not reflow, but it does fit: the
taskbar leaves 212 px of usable height, a default 200×120 window cascades in
18 px steps, and the launcher menu is 140 px wide. The fifth cascade position
reaches y=212, exactly the taskbar edge — so with several zapps open, later
windows are partly hidden behind the taskbar. Cosmetic, not broken, and the drag
clamp reads the screen size at runtime so windows can always be pulled back.

## Not yet run on hardware

Everything above is a build-and-reasoning result. The load path in particular --
buffer loader, Harvard heaps, shared-object extensions -- has not been executed
on silicon. Watch the console for `mounted /SD:`, `discovered N zapp(s)`, then
`llext: Loaded extension hello` and `[Hello] hello world, Zephyr!`.
