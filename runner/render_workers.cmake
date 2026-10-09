# Explicit per-target opt-in. Including runner.cmake alone adds no threads or
# link dependency to existing games. Also usable by ROM-free renderer tools.
include_guard(GLOBAL)
function(snesrecomp_target_render_workers target)
    if(NOT TARGET snesrecomp_render_workers)
        add_library(snesrecomp_render_workers STATIC
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/render_workers.c")
        target_compile_features(snesrecomp_render_workers PUBLIC c_std_11)
        target_include_directories(snesrecomp_render_workers PUBLIC
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src")
        if(NOT WIN32)
            find_package(Threads REQUIRED)
            target_link_libraries(snesrecomp_render_workers PUBLIC Threads::Threads)
        endif()
    endif()
    target_link_libraries(${target} PRIVATE snesrecomp_render_workers)
endfunction()
