# Contributing

## Getting a workspace

Prerequisites and the six commands are in the [README](README.md#try-it). This
repository *is* the west topdir — `west.yml` lives in `manifest/` so that it can
be — which means `west update` clones Zephyr and the modules into `zephyr/` and
`modules/` inside the checkout. Both are gitignored.

## The one command

```sh
tools/ci-check.sh
```

Three boards built pristine, both QEMU images run, every `.llext`'s imports
checked with the right `nm` per architecture, the Kconfig warnings read per
board, and an exit status that is the number of failed checks. Run it before
believing a change is done. `.github/workflows/build.yml` is a thin caller, so
anything that fails in CI reproduces locally with that line.

The fast inner loop, while iterating:

```sh
ZD_CI_BOARDS=qemu ZD_CI_PRISTINE=0 tools/ci-check.sh
```

Its pieces, when you want just one:

```sh
# Drive the image headless: click, type, assert on the console.
python3 tools/qemu-drive.py -d build 'click:30,258' 'wait:1' 'type:hello' 'key:ctrl-s'

# Launch every zapp, close every window, prove the counters came back.
west build -b qemu_cortex_a53 app -- -DEXTRA_CONF_FILE=smoke.conf
```

## Three habits this project learned expensively

**Read build output unfiltered.** A `grep` for `error` once hid a failure, and
QEMU then happily ran the previous ELF and reported a pass.

**Read the Kconfig warnings, per board.** A symbol that does not exist on a
target is a symbol that silently does nothing, and the build says so every time.
`CONFIG_LLEXT_HEAP_SIZE` does not exist on the Harvard ESP32-S3; setting it
globally warned on every CoreS3 build for a milestone while the board ran out of
exactly that heap. `ci-check.sh` now fails on the warning — which only works
because nobody generates one on purpose.

Note how that warning is worded, because the obvious grep does not match it. It
reads `was assigned the value '384' but got` / `the value ''. Check these
unsatisfied dependencies: (!HARVARD) (=n).` — **wrapped**, at about 100 columns.
Match `was assigned the value` and read the next two lines.

**A check that has never failed is not known to work.** The Kconfig check above
was written matching a phrase that could never appear, and passed on a build
that was warning. Break the thing on purpose once and watch the check fire.

## Rules that are easy to get wrong

Each of these has its own section in
[docs/architecture.md](docs/architecture.md); this is the index.

- **Never destroy during dispatch.** Everything goes through the deferred reap
  in `app/src/main.c`, and the drain order there is load-bearing. Opening is
  deferred too, not just destroying. A surface rebuilt often enough wants a
  model rather than another deferral.
- **The WM's `sys_dlist_t` is the truth for z-order**, not LVGL's child order.
  Same for geometry, and for a list's contents.
- **Keys are queued.** Touching LVGL from Zephyr's input thread is the bug the
  queue exists to prevent. A new input modality inherits it.
- **The ABI grows by appending only** — the vtable, `enum zd_event_type` and the
  `ZD_KEY_*` constants alike. New values go on the end, never beside the ones
  they belong with. Offsets are the compatibility rule; `sizeof(struct
  zd_event)` deliberately is not.
- **Hit slop does not compose.** Adjacent controls must be bigger, not have
  bigger hit areas.
- **Never read a size back off LVGL** to compute the next thing's position; it
  has not laid out yet.
- **Zapps never see an `lv_obj_t`**, never read `CONFIG_*`, and have no libc.
  See [docs/writing-a-zapp.md](docs/writing-a-zapp.md).

## Adding a zapp

[docs/writing-a-zapp.md](docs/writing-a-zapp.md). Two lines of
`app/CMakeLists.txt`, and check `nm -u` on the result before you believe it.

## Conventions

- **`[A]`–`[N]` in a comment is a milestone letter**, indexing
  [docs/history.md](docs/history.md). They are provenance, not open work — a
  comment saying "wired to the loader at milestone F" means it has been wired
  since then, not that it is pending.
- **SPDX headers on every source file**: `SPDX-License-Identifier: Apache-2.0`.
  In Python, at the end of the module docstring.
- **Comments explain why, not what.** The density in `app/src/` is deliberate:
  most of those paragraphs are a bug that already happened once.
- **`CLAUDE.md`** is orientation for AI coding agents working in this
  repository. It restates the rules above; if the two disagree, this file and
  `docs/` are correct.

## Verification before a release

`tools/mkrelease.sh` builds all three boards pristine, assembles the per-board
SD-card tree, and writes `SHA256SUMS`. It refuses to run on a dirty tree,
because the boot banner takes its build id from `git describe` and a dirty tree
makes that banner name a commit nobody else can obtain.
