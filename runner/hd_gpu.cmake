# Optional presenter for owned HD jobs. Core/portable jobs have no SDL/GL
# dependency. Hosts with their own GL loader declare EXTERNAL_GL_LOADER.
include_guard(GLOBAL)
function(snesrecomp_target_hd_jobs target)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/snes/hd_jobs.c")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src")
    if(NOT MSVC)
        target_link_libraries(${target} PRIVATE m)
    endif()
endfunction()
function(snesrecomp_target_hd_gpu target)
    cmake_parse_arguments(HD "EXTERNAL_GL_LOADER" "" "" ${ARGN})
    option(SNESRECOMP_HD_GPU "Build the optional desktop HD GPU consumer" ON)
    snesrecomp_target_hd_jobs(${target})
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/desktop/hd_gpu.c")
    target_compile_definitions(${target} PRIVATE SNESRECOMP_HD_GPU=$<BOOL:${SNESRECOMP_HD_GPU}>)
    if(SNESRECOMP_HD_GPU)
        snesrecomp_target_sdl(${target})
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/desktop"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/gl_core")
        if(NOT HD_EXTERNAL_GL_LOADER)
            target_sources(${target} PRIVATE
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/gl_core/gl_core_3_1.c")
        endif()
        find_package(OpenGL REQUIRED)
        target_link_libraries(${target} PRIVATE OpenGL::GL)
    endif()
endfunction()
