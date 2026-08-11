cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_desktop LANGUAGES C)

set(ZD_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
set(ZD_ZAPPS   ${CMAKE_CURRENT_SOURCE_DIR}/../zapps)

# include/zd/ lives outside app/ on purpose: it is the app ABI, shared verbatim
# with every extension, and it must survive apps moving to an out-of-tree build
# against the llext EDK.
target_include_directories(app PRIVATE ${ZD_INCLUDE})

target_sources(app PRIVATE
  src/main.c
  src/chrome/theme.c
  src/chrome/titlebar.c
  src/shell/desktop.c
  src/shell/taskbar.c
  src/shell/launcher.c
  src/host/session.c
  src/host/storage.c
  src/host/fs_shim.c
  src/host/host_api.c
  src/loader/zapp_loader.c
  src/loader/zapp_instance.c
  src/wm/wm.c
  src/wm/client.c
  src/wm/stack.c
  src/wm/focus.c
  src/wm/drag.c
  src/wm/handle.c
)

target_sources_ifdef(CONFIG_ZD_SELFTEST app PRIVATE src/selftest.c)
target_sources_ifdef(CONFIG_ZD_SEED_BUILTIN_ZAPPS app PRIVATE src/loader/seed.c)

# --- zapps --------------------------------------------------------------------
#
# Desktop apps live in zapps/ rather than apps/, so that nothing is confused with
# Zephyr's own convention where app/ is the application source directory -- which
# this project also has, one level up.
#
# They are built by this CMake project but are separate ELF artifacts from
# zephyr.elf: one `west build` produces both the desktop image and hello.llext.
# ARM/ARM64 default to LLEXT_TYPE_ELF_OBJECT, which permits exactly one source
# file per extension -- fine for hello world, and lifted later by switching to
# LLEXT_TYPE_ELF_RELOCATABLE.
#
# The eventual move to genuinely out-of-tree app builds is
# `west build -t llext-edk` plus an independent CMake project. Keeping the ABI
# header outside app/ is what makes that a move rather than a rewrite.

# Each app is one source file: ARM/ARM64 default to LLEXT_TYPE_ELF_OBJECT.
set(ZD_ZAPP_NAMES hello notes badabi)

foreach(zd_app ${ZD_ZAPP_NAMES})
  set(ZD_${zd_app}_LLEXT ${PROJECT_BINARY_DIR}/${zd_app}.llext)
  add_llext_target(zd_zapp_${zd_app}
    OUTPUT  ${ZD_${zd_app}_LLEXT}
    SOURCES ${ZD_ZAPPS}/${zd_app}/${zd_app}.c
  )
  llext_include_directories(zd_zapp_${zd_app} ${ZD_INCLUDE})

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
    DEPENDS ${ZD_ZAPPS}/${zd_app}/${zd_app}.c ${ZD_INCLUDE}/zd/zapp_abi.h
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
