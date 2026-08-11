cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_desktop LANGUAGES C)

set(ZD_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
set(ZD_APPS    ${CMAKE_CURRENT_SOURCE_DIR}/../apps)

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
  src/loader/app_loader.c
  src/loader/app_instance.c
  src/loader/seed.c
  src/wm/wm.c
  src/wm/client.c
  src/wm/stack.c
  src/wm/focus.c
  src/wm/drag.c
  src/wm/handle.c
)

# --- apps ---------------------------------------------------------------------
#
# Apps are built by this CMake project but are separate ELF artifacts from
# zephyr.elf: one `west build` produces both the desktop image and hello.llext.
# ARM/ARM64 default to LLEXT_TYPE_ELF_OBJECT, which permits exactly one source
# file per extension -- fine for hello world, and lifted later by switching to
# LLEXT_TYPE_ELF_RELOCATABLE.
#
# The eventual move to genuinely out-of-tree app builds is
# `west build -t llext-edk` plus an independent CMake project. Keeping the ABI
# header outside app/ is what makes that a move rather than a rewrite.

set(ZD_HELLO_LLEXT ${PROJECT_BINARY_DIR}/hello.llext)

add_llext_target(zd_app_hello
  OUTPUT  ${ZD_HELLO_LLEXT}
  SOURCES ${ZD_APPS}/hello/hello.c
)
llext_include_directories(zd_app_hello ${ZD_INCLUDE})

# Embed the built extension so the desktop can install it to /system/apps on
# first boot. Everything downstream still reads it back off the filesystem.
#
# generate_inc_file_for_target (not ..._for_gen_target) is deliberate: it wraps
# the .inc in its own custom target and makes `app` depend on *that*. Passing
# zd_app_hello instead makes app depend on the .llext but not on the .inc, and
# seed.c then races the generator.
generate_inc_file_for_target(app
  ${ZD_HELLO_LLEXT}
  ${ZEPHYR_BINARY_DIR}/include/generated/hello_llext.inc
)
