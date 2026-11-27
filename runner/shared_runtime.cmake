# Shared service implementations. SNES serializers, scheduling and UI remain
# in their existing adapters; repository-relative pins are the default.
set(RECOMP_SHARED_CORE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../external/shared-cores"
    CACHE PATH "Pinned shared runtime service source root")
set(_snes_shared_saved_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
set(RECOMP_INPUT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
foreach(_provider mod persistence diagnostics)
    if(NOT TARGET recomp_${_provider}_core)
        add_subdirectory("${RECOMP_SHARED_CORE_ROOT}/recomp-${_provider}-core"
            "${CMAKE_BINARY_DIR}/shared-cores/recomp-${_provider}-core" EXCLUDE_FROM_ALL)
    endif()
endforeach()
set(BUILD_TESTING "${_snes_shared_saved_testing}")
list(APPEND SNESRECOMP_RUNNER_LIBRARIES
    recomp-mod-core::core recomp-persistence-core::core
    recomp-diagnostics-core::core)
