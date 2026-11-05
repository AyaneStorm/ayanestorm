# -*- cmake -*-
# AyaneStorm renderer. All its build logic stays in this file, in
# indra/asrenderer/CMakeLists.txt and in indra/newview/asr_sources.cmake, so
# upstream's CMake files need only a few lines and merges stay clean. It is off
# by default, which leaves the OpenGL viewer build unchanged. build_nolto.cmd
# turns it on. See doc/ayanestorm-new-renderer-plan.md.
option(AS_RENDERER "Build the AyaneStorm renderer library" OFF)

if(AS_RENDERER)
    add_subdirectory(${LIBS_OPEN_PREFIX}asrenderer)
endif()
