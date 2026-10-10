# DSP-1 implementation is fixed in the binary. Firmware is an LLE input,
# never an implementation selector. Keep separate build directories per choice.
set(SNESRECOMP_DSP1_IMPL "LLE" CACHE STRING "DSP-1 implementation (LLE or HLE)")
set_property(CACHE SNESRECOMP_DSP1_IMPL PROPERTY STRINGS LLE HLE)
if(NOT SNESRECOMP_DSP1_IMPL STREQUAL "LLE" AND
   NOT SNESRECOMP_DSP1_IMPL STREQUAL "HLE")
    message(FATAL_ERROR "SNESRECOMP_DSP1_IMPL must be LLE or HLE (got '${SNESRECOMP_DSP1_IMPL}')")
endif()
if(SNESRECOMP_DSP1_IMPL STREQUAL "HLE")
    add_compile_definitions(SNESRECOMP_DSP1_HLE=1)
else()
    add_compile_definitions(SNESRECOMP_DSP1_HLE=0)
endif()
message(STATUS "snesrecomp: DSP-1 implementation = ${SNESRECOMP_DSP1_IMPL} (build-fixed)")
