cmake_minimum_required(VERSION 3.16)
project(data_pack_tests C CXX)
enable_testing()
include(../../runner/data_packs.cmake)
add_executable(data_pack_probe probe.c)
snesrecomp_target_data_packs(data_pack_probe)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME data_pack_catalog COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/check_catalog.py $<TARGET_FILE:data_pack_probe>)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME sram_publication COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_sram.py ${CMAKE_C_COMPILER})
endif()

add_test(NAME packs_disabled_by_default COMMAND ${CMAKE_COMMAND}
    -S ${CMAKE_CURRENT_SOURCE_DIR}/no_opt_in -B ${CMAKE_CURRENT_BINARY_DIR}/no_opt_in
    -G "${CMAKE_GENERATOR}" -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})

add_executable(frame_dump_opt_in frame_dump_opt_in.c ../../runner/src/framedump.c)
target_include_directories(frame_dump_opt_in PRIVATE ../../runner/src)
add_test(NAME frame_dump_opt_in COMMAND frame_dump_opt_in)
