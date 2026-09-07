/**
 * @file asvignette.h
 * @author chanayane@firestorm
 * @brief Viewer-local configurable screen-space vignette.
 */

#ifndef AS_VIGNETTE_H
#define AS_VIGNETTE_H

#include <vector>

#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASVignette
{
    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();
    // Original direct-framebuffer path used when color grading is inactive.
    void render(S32 width, S32 height, LLVertexBuffer& screen_triangle);
    // Composites directly into color_target without sampling it.
    void render(LLRenderTarget& color_target, LLVertexBuffer& screen_triangle);
}

#endif
