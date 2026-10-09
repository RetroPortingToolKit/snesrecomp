cmake_minimum_required(VERSION 3.20)
project(SnesStandaloneSaveMenu C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(SNESRECOMP_ENABLE_NET OFF CACHE BOOL "" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/../../../runner/runner.cmake")

# Exercise the source list consumers use, without the netplay target helper
# or any network library. The browser must still link and save/load locally.
set(_sources ${SNESRECOMP_RUNNER_SOURCES})
list(FILTER _sources INCLUDE REGEX "/(snes_savestate_menu|snes_overlay_draw|snes_netplay)\\.c$")
add_executable(standalone_save_menu ../standalone_save_menu_test.c ${_sources})
target_include_directories(standalone_save_menu PRIVATE ${SNESRECOMP_RUNNER_INCLUDE_DIRS})
snesrecomp_target_sdl(standalone_save_menu)
enable_testing()
add_test(NAME standalone_save_menu COMMAND standalone_save_menu)
