# -*- cmake -*-
# AyaneStorm renderer. All its build logic stays in this file and in
# indra/asrenderer/CMakeLists.txt, so upstream's CMake files need a single line
# and merges stay clean. It is off until the viewer uses it in P2, which leaves
# the OpenGL viewer build unchanged. See doc/ayanestorm-new-renderer-plan.md.
option(AS_RENDERER "Build the AyaneStorm renderer library" OFF)

if(AS_RENDERER)
    add_subdirectory(${LIBS_OPEN_PREFIX}asrenderer)
endif()
