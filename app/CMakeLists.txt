cmake_minimum_required(VERSION 3.20.0)

# --- QEMU devices -------------------------------------------------------------
#
# A keyboard, which the board does not ask for: Zephyr's own support for
# qemu_cortex_a53 only needs a pointer, so its board.cmake stops at a
# virtio-tablet on bus 3. The same virtio input driver handles keys, so one more
# device is the whole of it. The bus index must match the virtio_mmio4 node in
# boards/qemu_cortex_a53.overlay, which is the transport at 0xa000800.
#
# This has to be here, ABOVE find_package(Zephyr), and it has to be a CMake
# variable. Two routes that look better and are not:
#
#   - list(APPEND QEMU_EXTRA_FLAGS ...) further down this file never arrives.
#     cmake/emu/qemu.cmake is included from zephyr/CMakeLists.txt during
#     find_package(), so the run target is fully assembled before the
#     application's own body is read.
#   - CONFIG_QEMU_EXTRA_FLAGS in the board fragment reaches qemu.cmake, but the
#     value contains "bus=", and scripts/build/llext_inspect_discarded_groups.py
#     parses .config with line.split("=") and no maxsplit. The build then dies
#     in a post-link step with "ValueError: too many values to unpack".
#     Upstream bug at Zephyr main e201b84b; any CONFIG_ string holding a second
#     '=' trips it.
if(BOARD STREQUAL "qemu_cortex_a53")
  list(APPEND QEMU_EXTRA_FLAGS -device virtio-keyboard-device,bus=virtio-mmio-bus.4)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_desktop LANGUAGES C)

set(ZD_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
set(ZD_ZAPPS   ${CMAKE_CURRENT_SOURCE_DIR}/../zapps)

# --- build identity -----------------------------------------------------------
#
# app/VERSION gives APP_VERSION_STRING through <zephyr/app_version.h>; this adds
# which commit it was cut from, so a release image says "v0.1.0" and everything
# else says "v0.1.0-3-gabc123def456". The first console line prints both, because
# "is the board running the image I just built?" is otherwise unanswerable, and
# milestone J hand-tested a stale binary twice for exactly that reason.
#
# Two honest limits, stated rather than papered over:
#
#   - this identifies the COMMIT, not the working tree. Uncommitted edits are
#     invisible here; the version line tells you which release you are near, not
#     that your last save made it in.
#   - it is evaluated at CMake configure time. The CMAKE_CONFIGURE_DEPENDS below
#     re-runs configure when HEAD or the index moves, which covers commit,
#     checkout and rebase -- the cases where the string would otherwise be a
#     confident lie.
include(git)
git_describe(${CMAKE_CURRENT_SOURCE_DIR} ZD_BUILD_ID)
if(NOT ZD_BUILD_ID)
  set(ZD_BUILD_ID "no-git")
endif()
zephyr_compile_definitions(ZD_BUILD_ID="${ZD_BUILD_ID}")

foreach(git_file HEAD index)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../.git/${git_file})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${CMAKE_CURRENT_SOURCE_DIR}/../.git/${git_file})
  endif()
endforeach()

# include/zd/ lives outside app/ on purpose: it is the app ABI, shared verbatim
# with every extension, and it must survive apps moving to an out-of-tree build
# against the llext EDK.
target_include_directories(app PRIVATE ${ZD_INCLUDE})

target_sources(app PRIVATE
  src/main.c
  src/chrome/theme.c
  src/chrome/titlebar.c
  src/chrome/menu.c
  src/chrome/rowlist.c
  src/chrome/cellgrid.c
  src/shell/desktop.c
  src/shell/taskbar.c
  src/shell/launcher.c
  src/shell/tasklist.c
  src/shell/dialog.c
  src/host/session.c
  src/host/storage.c
  src/host/fs_shim.c
  src/host/fs_api.c
  src/host/host_api.c
  src/host/grid_api.c
  src/host/list_api.c
  src/host/text_api.c
  src/host/timer_api.c
  src/host/clipboard.c
  src/host/clock.c
  src/loader/zapp_loader.c
  src/loader/zapp_instance.c
  src/input/keys.c
  src/input/keymap.c
  src/wm/wm.c
  src/wm/keys.c
  src/wm/client.c
  src/wm/stack.c
  src/wm/focus.c
  src/wm/drag.c
  src/wm/handle.c
)

target_sources_ifdef(CONFIG_ZD_SPI_DC_MISO_ARBITER app PRIVATE src/host/bus_arb.c)
target_sources_ifdef(CONFIG_ZD_OSK app PRIVATE src/shell/osk.c)
target_sources_ifdef(CONFIG_ZD_SELFTEST app PRIVATE src/selftest.c)
target_sources_ifdef(CONFIG_ZD_SMOKE_TEST app PRIVATE src/smoke.c)
target_sources_ifdef(CONFIG_ZD_TOUCH_PROBE app PRIVATE src/touch_probe.c)
target_sources_ifdef(CONFIG_ZD_SEED_BUILTIN_ZAPPS app PRIVATE src/loader/seed.c)

# --- zapps --------------------------------------------------------------------
#
# Desktop apps live in zapps/ rather than apps/, so that nothing is confused with
# Zephyr's own convention where app/ is the application source directory -- which
# this project also has, one level up.
#
# They are built by this CMake project but are separate ELF artifacts from
# zephyr.elf: one `west build` produces both the desktop image and hello.llext.
# A zapp may now be several source files. ARM and ARM64 default to
# LLEXT_TYPE_ELF_OBJECT, which is a single compiler invocation and therefore
# exactly one file; the board fragments for those targets select
# LLEXT_TYPE_ELF_RELOCATABLE instead, and Xtensa already defaults to
# LLEXT_TYPE_ELF_SHAREDLIB, which never had the restriction. add_llext_target
# hard-errors on more than one source under ELF_OBJECT, so a board fragment
# that forgets fails at CMake time rather than silently.
#
# The eventual move to genuinely out-of-tree app builds is
# `west build -t llext-edk` plus an independent CMake project. Keeping the ABI
# header outside app/ is what makes that a move rather than a rewrite.

set(ZD_ZAPP_NAMES hello notes badabi notepad files mines)

# hello stays a single translation unit on purpose: it is the proof that the
# simplest possible zapp is still the simplest possible zapp, and it is the only
# thing that would notice if ELF_OBJECT came back. Everything else links
# zapps/lib, so the no-libc helpers exist once rather than once per zapp.
set(ZD_hello_SOURCES  ${ZD_ZAPPS}/hello/hello.c)
set(ZD_notes_SOURCES  ${ZD_ZAPPS}/notes/notes.c ${ZD_ZAPPS}/lib/zapplib.c)
set(ZD_badabi_SOURCES ${ZD_ZAPPS}/badabi/badabi.c)
set(ZD_notepad_SOURCES
  ${ZD_ZAPPS}/notepad/notepad.c
  ${ZD_ZAPPS}/notepad/commands.c
  ${ZD_ZAPPS}/notepad/document.c
  ${ZD_ZAPPS}/lib/zapplib.c
)
set(ZD_files_SOURCES
  ${ZD_ZAPPS}/files/files.c
  ${ZD_ZAPPS}/files/browse.c
  ${ZD_ZAPPS}/files/commands.c
  ${ZD_ZAPPS}/lib/zapplib.c
)
set(ZD_mines_SOURCES
  ${ZD_ZAPPS}/mines/mines.c
  ${ZD_ZAPPS}/mines/board.c
  ${ZD_ZAPPS}/mines/render.c
  ${ZD_ZAPPS}/lib/zapplib.c
)

foreach(zd_app ${ZD_ZAPP_NAMES})
  set(ZD_${zd_app}_LLEXT ${PROJECT_BINARY_DIR}/${zd_app}.llext)
  add_llext_target(zd_zapp_${zd_app}
    OUTPUT  ${ZD_${zd_app}_LLEXT}
    SOURCES ${ZD_${zd_app}_SOURCES}
  )
  # ZD_INCLUDE for <zd/zapp_abi.h>; ZD_ZAPPS for <lib/zapplib.h>, spelled with
  # the directory so it can never be mistaken for a system header.
  llext_include_directories(zd_zapp_${zd_app} ${ZD_INCLUDE} ${ZD_ZAPPS})

  # WORKAROUND (Zephyr main @ e201b84b): add_llext_target's packaging step does
  # not re-run when a source file changes. Its chain is
  #   source -> .obj -> <target>_llext_proc (a custom TARGET) -> _debug.elf -> .llext
  # and the _debug.elf command DEPENDS only on that phony target, with no
  # file-level dependency on the object. So the .obj recompiles, the copy never
  # re-runs, and the .llext silently stays stale -- an app edit ships the
  # PREVIOUS binary with no warning. Verified: repeated incremental builds after
  # editing an app leave the .llext untouched; only `west build -p` refreshes it.
  #
  # Re-attach the missing file dependency to the documented pkg_input property.
  get_target_property(zd_pkg_input zd_zapp_${zd_app} pkg_input)
  add_custom_command(OUTPUT ${zd_pkg_input} APPEND
    DEPENDS ${ZD_${zd_app}_SOURCES} ${ZD_INCLUDE}/zd/zapp_abi.h
            ${ZD_ZAPPS}/lib/zapplib.h
  )

  generate_inc_file_for_target(app
    ${ZD_${zd_app}_LLEXT}
    ${ZEPHYR_BINARY_DIR}/include/generated/${zd_app}_llext.inc
  )
endforeach()

# Embed the built extension so the desktop can install it to /system/apps on
# first boot. Everything downstream still reads it back off the filesystem.
#
# Note: generate_inc_file_for_target, not ..._for_gen_target. The latter makes
# `app` depend on the .llext but not on the generated .inc, and seed.c then
# races the generator.
