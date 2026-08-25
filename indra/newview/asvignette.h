/**
 * @file asvignette.h
 * @author chanayane@firestorm
 * @brief Viewer-local configurable screen-space vignette.
 */

#ifndef AS_VIGNETTE_H
#define AS_VIGNETTE_H

#include <vector>

#include "llglslshader.h"

class LLVertexBuffer;

namespace ASVignette
{
    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();
    void render(S32 width, S32 height, LLVertexBuffer& screen_triangle);
}

#endif
