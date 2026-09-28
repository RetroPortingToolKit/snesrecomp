# Explicit per-target opt-in: no catalog, UI, directory or dependency is added
# to games that do not call this helper. May also be included without runner.cmake.
set(_SNES_DATA_PACK_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/src")
function(snesrecomp_target_data_packs target)
    if(NOT TARGET snesrecomp_data_packs)
        enable_language(CXX)
        find_package(LibArchive REQUIRED)
        find_path(SNESRECOMP_RAPIDJSON_INCLUDE_DIR rapidjson/document.h REQUIRED)
        add_library(snesrecomp_data_packs STATIC
            "${_SNES_DATA_PACK_SOURCE_DIR}/data_pack.cpp"
            "${_SNES_DATA_PACK_SOURCE_DIR}/sha256.c")
        set_target_properties(snesrecomp_data_packs PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        target_compile_features(snesrecomp_data_packs PUBLIC cxx_std_17)
        target_include_directories(snesrecomp_data_packs PUBLIC "${_SNES_DATA_PACK_SOURCE_DIR}")
        target_include_directories(snesrecomp_data_packs SYSTEM PUBLIC "${SNESRECOMP_RAPIDJSON_INCLUDE_DIR}")
        target_link_libraries(snesrecomp_data_packs PUBLIC LibArchive::LibArchive)
    endif()
    target_link_libraries(${target} PRIVATE snesrecomp_data_packs)
endfunction()
