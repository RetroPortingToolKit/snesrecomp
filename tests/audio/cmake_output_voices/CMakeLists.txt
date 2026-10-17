cmake_minimum_required(VERSION 3.20)
project(snes_dsp_output_voices C)
set(CMAKE_C_STANDARD 11)
enable_testing()
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
foreach(variant IN ITEMS default trace0 trace1 cosim)
  add_executable(dsp_output_${variant}
    "${ROOT}/tests/audio/dsp_output_voices_test.c"
    "${ROOT}/runner/src/snes/dsp.c"
    "${ROOT}/runner/src/snes/dsp_shadow.c"
    "${ROOT}/runner/src/snes/audio_shadow.c"
    "${ROOT}/runner/src/audio_trace.c")
  target_include_directories(dsp_output_${variant} PRIVATE "${ROOT}/runner/src")
  if(NOT MSVC)
    target_compile_options(dsp_output_${variant} PRIVATE -Wall -Wextra -Werror -Wno-unused-function -ffunction-sections -fdata-sections)
    target_link_options(dsp_output_${variant} PRIVATE -Wl,--gc-sections)
    target_link_libraries(dsp_output_${variant} PRIVATE m)
  endif()
  if(variant STREQUAL trace0)
    target_compile_definitions(dsp_output_${variant} PRIVATE SNESRECOMP_TRACE=0)
  elseif(variant STREQUAL trace1)
    target_compile_definitions(dsp_output_${variant} PRIVATE SNESRECOMP_TRACE=1)
  elseif(variant STREQUAL cosim)
    target_compile_definitions(dsp_output_${variant} PRIVATE SNESRECOMP_TRACE=0 SNES_COSIM=1)
  endif()
  add_test(NAME dsp_output_${variant} COMMAND dsp_output_${variant})
endforeach()
