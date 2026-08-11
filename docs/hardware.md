# Hardware: MIMXRT1060-EVK

The hardware checkpoint is deliberately **headless**. No panel is attached yet,
so the desktop runs against a dummy display controller: the WM, the loader, the
filesystem and the app ABI are all exercised on real Cortex-M7 silicon, and only
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

- `CONFIG_ZD_FS_ROOT="/SD:"` instead of `/RAM:`. Apps never notice — they resolve
  directories through the session, which is why that indirection exists.
- SD card instead of a RAM disk.
- **`CONFIG_ZD_SEED_BUILTIN_APPS=n`.** The point of this checkpoint is that a
  `.llext` arrives from *outside*, so the desktop must find apps it did not write
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

That alone is the checkpoint: a filesystem the desktop did not create, apps it
did not install, and the permission and handle checks passing on real silicon.

To prove load/unload without a pointer, temporarily launch from `main()` after
`zd_launcher_init()`:

```c
struct zd_app_entry entry[ZD_MAX_DISCOVERED];
int n = zd_apps_discover(&session, entry, ZD_MAX_DISCOVERED);
if (n > 0) {
        zd_app_launch(&entry[0]);
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
