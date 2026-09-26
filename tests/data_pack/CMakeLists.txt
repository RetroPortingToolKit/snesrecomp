cmake_minimum_required(VERSION 3.16)
project(data_pack_tests C CXX)
enable_testing()
include(../../runner/data_packs.cmake)
add_executable(data_pack_probe probe.c)
snesrecomp_target_data_packs(data_pack_probe)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME data_pack_catalog COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_catalog.py $<TARGET_FILE:data_pack_probe>)
