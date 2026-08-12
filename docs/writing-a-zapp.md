# Writing a zapp

A zapp is an ELF relocatable (`.llext`) that the desktop finds on a filesystem
and loads at runtime. It exports exactly one symbol and imports at most one. It
never sees an `lv_obj_t`, includes no Zephyr and no LVGL headers, and has no
idea what draws its window.

This walks through building one. [abi.md](abi.md) is the reference for what you
can call once it runs.

## The whole of a zapp

```c
#include <zephyr/llext/symbol.h>
#include <zd/zapp_abi.h>

static const struct zd_host_api *host;

static int my_init(zd_zapp_ctx_t ctx, const struct zd_host_api *api)
{
        struct zd_window_desc desc = {
                .title = "My zapp",
                .geom = { 0, 0, 0, 0 },   /* let the desktop place us */
        };

        host = api;

        zd_window_t win = api->window_create(ctx, &desc);
        if (win == NULL) {
                return -1;
        }
        api->label_create(ctx, win, "hello", 8, 8);

        api->set_user_data(ctx, (void *)win);
        return 0;
}

struct zd_zapp_manifest zd_zapp_manifest = {
        .magic     = ZD_ZAPP_MAGIC,
        .abi_major = ZD_ABI_MAJOR,
        .abi_minor = ZD_ABI_MINOR,
        .name      = "My zapp",
        .init      = my_init,
};
LL_EXTENSION_SYMBOL(zd_zapp_manifest);
```

`init` is required; `event` and `fini` are optional. Returning non-zero from
`init` refuses the launch and the desktop unwinds everything you created.

## Building it

Put it in `zapps/<name>/<name>.c`, then two edits in `app/CMakeLists.txt`:

```cmake
set(ZD_ZAPP_NAMES hello notes badabi notepad files mines mine)

set(ZD_mine_SOURCES ${ZD_ZAPPS}/mine/mine.c ${ZD_ZAPPS}/lib/zapplib.c)
```

`west build -b qemu_cortex_a53 app` now produces `build/mine.llext` alongside
`zephyr.elf`, and the boot seeder writes it to `/system/zapps/` so the launcher
finds it. A zapp may be as many source files as you like — `notepad` is four.

## Copy hello, but not all of it

[`zapps/hello/hello.c`](../zapps/hello/hello.c) is the smallest working zapp and
the right thing to start from. Two things in it are **not** part of the pattern:

- **The `zd_get_host_api()` call.** A zapp imports nothing; `init()` is handed
  the table. That call is a deliberate loader self-test living in the one zapp
  everybody reads. Copying it gives your zapp an import it does not need.
- **The `if (ev->win != mine) return;` guard at the top of `event()`.** Correct
  for hello, which only ever receives window events. `ZD_EV_TIMER` arrives with
  `ev->win == NULL`, so if you start a timer this guard silently eats it. Switch
  on `ev->type` first.

## Four traps, all of which build cleanly and fail later

### There is no libc

`CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n` leaves the desktop exporting exactly one
symbol, so `strlen`, `memcpy` and `snprintf` are not there to link against.

The nasty half is what you did not write. **GCC synthesises a `memset` call out
of an ordinary struct assignment**, and `struct zd_dirent ent = {0};` is enough
to do it. That compiles without a murmur and fails at load with an
undefined-symbol error a long way from the cause.

[`zapps/lib/zapplib.h`](../zapps/lib/zapplib.c) has replacements — `z_zero`,
`z_strlen`, `z_copy`, number formatting — written through `volatile` pointers
precisely so the optimiser cannot turn them back into the call you were avoiding.

### 64-bit division, on one architecture only

```c
uint16_t secs = (api->uptime_ms() - started) / 1000;   /* fine on arm64 */
```

One instruction on arm64; on 32-bit Xtensa it becomes a call to libgcc's
`__divdi3`, which a zapp may not import. Narrow to `uint32_t` before dividing.
Same family as the synthesised `memset`: the compiler emitting a call nobody
wrote, and this one exists on exactly one of the supported boards.

### Check what you actually import

```sh
nm -u build/<name>.llext            # expect at most zd_get_host_api
nm -D -u build-cores3/<name>.llext  # Xtensa: shared object, so -D
```

The ARM builds are where this check is sharp. Xtensa reports a pre-existing
spurious `memset` that does not stop the board loading zapps, so treat anything
*else* there as the finding. `tools/ci-check.sh` runs this for every zapp on
every board.

### Never read `CONFIG_*`

Zapps compile with `-imacros autoconf.h`, so `CONFIG_ZD_TEXT_MAX` is right there
and works. Using it welds your zapp to one desktop build, which is the single
thing `include/zd/` exists to prevent. Ask through the ABI instead —
`text_get_capacity()`, `list_get_capacity()`, `grid_get_capacity()`,
`window_get_content_size()` — or carry your own constant.

## Two things that are not traps but surprise everyone

### Instances of one zapp share `.bss`

llext loads an image once and refcounts it, so launching your zapp twice gives
you two instances over **one** copy of your file-scope variables. A
`static zd_window_t window` is silently clobbered by whichever instance started
last.

Per-instance state goes through `set_user_data()` / `get_user_data()`. A
pointer-sized value rides in the slot directly; anything larger needs an
allocation you own and free in `fini()`. A `const struct zd_host_api *host` at
file scope *is* fine — every instance is handed the same table.

### Ask for your size; never assume one

The supported screens run from 320×240 to 480×272, and on a touch board the
on-screen keyboard can take 128 px of that while it is up. Nothing about your
window's size is knowable in advance.

`window_get_content_size()` tells you what you have. `window_set_geometry()` has
both a floor and a ceiling, so **asking for an absurd size and reading back what
arrived** is how you discover the largest window this desktop will give you.
Grids go further: `grid_fit()` and `grid_measure()` answer in cells, which is
how Minesweeper comes out 9×9 on one panel and 9×3 on another without knowing
either number.

## Where to go next

- [abi.md](abi.md) — every call, the ordering guarantees, and the rules about
  handles, events and what the desktop refuses to promise.
- [`zapps/notes/`](../zapps/notes) — the smallest zapp that touches storage.
- [`zapps/mines/`](../zapps/mines) — the largest, and the only one that is not
  an application: grids, timers, menus and dialogs.
- [using.md](using.md) — what your zapp's neighbours do.
