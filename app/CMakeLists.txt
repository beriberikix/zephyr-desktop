cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_desktop LANGUAGES C)

# include/zd/ lives outside app/ on purpose: it is the app ABI, shared verbatim
# with every extension, and it must survive apps moving to an out-of-tree build
# against the llext EDK.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_sources(app PRIVATE
  src/main.c
  src/chrome/theme.c
  src/chrome/titlebar.c
  src/shell/desktop.c
  src/shell/taskbar.c
  src/wm/wm.c
  src/wm/client.c
)
