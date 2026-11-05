# AyaneStorm renderer library. It has no viewer or OpenGL dependency, so it also
# builds on its own: cmake -S indra/asrenderer -B <build dir>
# Inside the viewer tree it is reached through indra/cmake/ASRenderer.cmake.
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(langs C CXX)
    if(APPLE)
        list(APPEND langs OBJCXX)
    endif()
    project(asrenderer LANGUAGES ${langs})
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ASR_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(ASR_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)

# --- Portable code, built on every platform. ---------------------------------
find_package(Threads REQUIRED)
add_library(asrenderer STATIC asrmemorybudget.cpp asrrenderthread.cpp asrscenefile.cpp
    asrtexturequeue.cpp asruilist.cpp)
target_include_directories(asrenderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asrenderer PUBLIC Threads::Threads)

# --- Graphics backend. Linux gets its surface code with the P8 Linux pass. ---
if(WIN32)
    target_sources(asrenderer PRIVATE asrdevicevulkan.cpp asrvma.cpp ${ASR_THIRD_PARTY}/volk/volk.c)
    target_include_directories(asrenderer PRIVATE
        ${ASR_THIRD_PARTY}/volk ${ASR_THIRD_PARTY}/vma ${ASR_THIRD_PARTY}/vulkan-headers/include)
    target_compile_definitions(asrenderer PRIVATE VK_USE_PLATFORM_WIN32_KHR ASR_HAS_DEVICE)
elseif(APPLE)
    target_sources(asrenderer PRIVATE asrdevicemetal.mm)
    set_source_files_properties(asrdevicemetal.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
    target_compile_definitions(asrenderer PRIVATE ASR_HAS_DEVICE)
    target_link_libraries(asrenderer PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
endif()

# --- Shader build step: one output per source, so editing a shader rebuilds
#     only that shader and no C++. ---------------------------------------------
find_program(ASR_GLSLANG NAMES glslang glslangValidator
    HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../../.glslang/bin $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin
    REQUIRED)
message(STATUS "asrenderer: glslang is ${ASR_GLSLANG}")

if(APPLE)
    # Converts SPIR-V to Metal shaders with the vendored SPIRV-Cross.
    add_executable(asr_spirv2msl tools/asrspirv2msl.cpp
        ${ASR_THIRD_PARTY}/spirv-cross/spirv_cross.cpp
        ${ASR_THIRD_PARTY}/spirv-cross/spirv_cfg.cpp
        ${ASR_THIRD_PARTY}/spirv-cross/spirv_cross_parsed_ir.cpp
        ${ASR_THIRD_PARTY}/spirv-cross/spirv_parser.cpp
        ${ASR_THIRD_PARTY}/spirv-cross/spirv_glsl.cpp
        ${ASR_THIRD_PARTY}/spirv-cross/spirv_msl.cpp)
    target_include_directories(asr_spirv2msl PRIVATE ${ASR_THIRD_PARTY}/spirv-cross)
endif()

# Adds the build rules for one GLSL file. The stage comes from the file name:
# ...V.glsl is vertex, ...F.glsl fragment, ...C.glsl compute.
function(asr_add_shader source outputs_var)
    get_filename_component(stem ${source} NAME_WE)
    string(REGEX MATCH ".$" suffix ${stem})
    if(suffix STREQUAL "V")
        set(stage vert)
    elseif(suffix STREQUAL "F")
        set(stage frag)
    elseif(suffix STREQUAL "C")
        set(stage comp)
    else()
        message(FATAL_ERROR "asrenderer: ${source} must end in V, F or C")
    endif()

    set(spv ${ASR_SHADER_OUTPUT_DIR}/${stem}.spv)
    add_custom_command(OUTPUT ${spv}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ASR_SHADER_OUTPUT_DIR}
        COMMAND ${ASR_GLSLANG} -V --target-env vulkan1.3 -S ${stage} -o ${spv} ${source}
        DEPENDS ${source} VERBATIM)
    set(result ${spv})

    if(APPLE)
        set(msl ${ASR_SHADER_OUTPUT_DIR}/${stem}.metal)
        set(lib ${ASR_SHADER_OUTPUT_DIR}/${stem}.metallib)
        add_custom_command(OUTPUT ${msl}
            COMMAND asr_spirv2msl ${spv} ${msl}
            DEPENDS ${spv} asr_spirv2msl VERBATIM)
        add_custom_command(OUTPUT ${lib}
            COMMAND xcrun metal -std=metal3.0 -o ${lib} ${msl}
            DEPENDS ${msl} VERBATIM)
        list(APPEND result ${lib})
    endif()

    set(${outputs_var} ${${outputs_var}} ${result} PARENT_SCOPE)
endfunction()

set(ASR_SHADER_OUTPUTS)
foreach(shader asrTestTriangleV asrTestTriangleF asrUIV asrUIF)
    asr_add_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.glsl ASR_SHADER_OUTPUTS)
endforeach()
add_custom_target(asrenderer_shaders ALL DEPENDS ${ASR_SHADER_OUTPUTS})

# --- Standalone test program: its builds never link the viewer. It is left out
#     of viewer builds, which do not need it. --------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(test)
endif()
