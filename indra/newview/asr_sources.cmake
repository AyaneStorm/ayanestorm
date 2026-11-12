# -*- cmake -*-
# The viewer's side of the AyaneStorm renderer. Keeping this here leaves two
# tagged lines in newview/CMakeLists.txt. See doc/ayanestorm-new-renderer-plan.md.
#
# Only asrviewer.cpp gets AS_RENDERER. A define on the whole viewer target would
# recompile every file in newview each time it is switched. The renderer's include
# folder does reach the whole target through the link, which is harmless.
list(APPEND viewer_SOURCE_FILES asravatarbody.cpp asrcameraturn.cpp asrtexturecache.cpp asrviewer.cpp asrworld.cpp)
list(APPEND viewer_HEADER_FILES asravatarbody.h asrcameraturn.h asrtexturecache.h asrviewer.h asrworld.h asrworldshared.h)

if(AS_RENDERER)
    set_source_files_properties(asravatarbody.cpp asrcameraturn.cpp asrtexturecache.cpp asrviewer.cpp asrworld.cpp PROPERTIES
        COMPILE_DEFINITIONS AS_RENDERER
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/../asrenderer)
endif()

function(asr_link_viewer viewer_target)
    if(AS_RENDERER)
        target_link_libraries(${viewer_target} $<LINK_ONLY:asrenderer>)
        add_dependencies(${viewer_target} asrenderer_shaders)
    endif()
endfunction()
