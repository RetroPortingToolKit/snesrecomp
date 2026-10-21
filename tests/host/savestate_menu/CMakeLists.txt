cmake_minimum_required(VERSION 3.20)
project(savestate_menu_tests C)
enable_testing()
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(SDL3 QUIET CONFIG)
if(SDL3_FOUND)
  set(SDL_TARGET SDL3::SDL3)
  set(SDL_VERSION 1)
else()
  find_package(SDL2 REQUIRED CONFIG)
  set(SDL_TARGET SDL2::SDL2)
  set(SDL_VERSION 0)
endif()
add_executable(savestate_menu_test
  ../savestate_menu_test.c
  "${REPO}/runner/src/snes_savestate_menu.c"
  "${REPO}/runner/src/snes_overlay_draw.c")
target_compile_features(savestate_menu_test PRIVATE c_std_11)
target_compile_definitions(savestate_menu_test PRIVATE
  SDL_MAIN_HANDLED SNESRECOMP_SDL3=${SDL_VERSION})
target_include_directories(savestate_menu_test PRIVATE
  "${REPO}/runner/src" "${REPO}/recompiler/runtime")
target_link_libraries(savestate_menu_test PRIVATE ${SDL_TARGET})
add_test(NAME savestate_menu COMMAND savestate_menu_test)
