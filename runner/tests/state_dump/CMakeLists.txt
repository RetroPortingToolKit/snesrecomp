cmake_minimum_required(VERSION 3.16)
project(snes_state_dump_contract C)
enable_testing()
if(NOT SNESRECOMP_TEST_ENGINE_ROOT)
  get_filename_component(SNESRECOMP_TEST_ENGINE_ROOT "../../.." ABSOLUTE)
endif()
add_executable(snes_state_dump_contract_test
  ../../../tests/state_dump/state_dump_test.c
  "${SNESRECOMP_TEST_ENGINE_ROOT}/runner/src/audio_trace.c")
target_include_directories(snes_state_dump_contract_test PRIVATE
  "${SNESRECOMP_TEST_ENGINE_ROOT}/runner/src")
target_compile_features(snes_state_dump_contract_test PRIVATE c_std_11)
if(MSVC)
  target_compile_options(snes_state_dump_contract_test PRIVATE /UNDEBUG)
else()
  target_compile_options(snes_state_dump_contract_test PRIVATE -UNDEBUG)
endif()
add_test(NAME snes_state_dump_contract COMMAND snes_state_dump_contract_test)
