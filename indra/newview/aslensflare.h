/**
 * @file aslensflare.h
 * @author chanayane@firestorm
 * @brief Viewer-local screen-space sun and moon lens flares.
 */

#ifndef AS_LENS_FLARE_H
#define AS_LENS_FLARE_H

#include <vector>

#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASLensFlare
{
    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();
    // Original direct-framebuffer path used when color grading is inactive.
    void render(LLRenderTarget& depth_target, LLVertexBuffer& screen_triangle);
    // Composites directly into color_target without sampling it.
    void render(LLRenderTarget& color_target, LLRenderTarget& depth_target, LLVertexBuffer& screen_triangle);
}

#endif
