# asrenderer_test: opens a window through the renderer with no viewer, and runs
# self checks. Linux builds the self checks only until its window code exists.
set(test_sources asrtestmain.cpp)
if(WIN32)
    list(APPEND test_sources asrtestwindowwin32.cpp asrtestvram.cpp)
elseif(APPLE)
    list(APPEND test_sources asrtestwindowcocoa.mm asrtestmouseprobe.mm asrtestvram.cpp)
    set_source_files_properties(asrtestwindowcocoa.mm asrtestmouseprobe.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
endif()

add_executable(asrenderer_test ${test_sources})
target_link_libraries(asrenderer_test PRIVATE asrenderer)
add_dependencies(asrenderer_test asrenderer_shaders)
target_compile_definitions(asrenderer_test PRIVATE ASR_SHADER_DIR="${ASR_SHADER_OUTPUT_DIR}")
if(WIN32 OR APPLE)
    target_compile_definitions(asrenderer_test PRIVATE ASR_HAS_DEVICE)
endif()
