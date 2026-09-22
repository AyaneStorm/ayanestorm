/**
 * @file asdepthoffield.h
 * @author chanayane@firestorm
 * @brief AyaneStorm-owned cinematic depth-of-field renderer.
 */
#ifndef AS_DEPTH_OF_FIELD_H
#define AS_DEPTH_OF_FIELD_H

#include <vector>

#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASDepthOfField
{
    void registerUICallbacks();
    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool createShaders(S32 shader_level);
    void unloadShaders();
    void releaseResources();

    // Returns true only after the advanced renderer has written destination.
    // The caller must continue through the legacy path when this returns false.
    bool render(LLRenderTarget& source, LLRenderTarget& destination,
                LLRenderTarget& depth, LLVertexBuffer& screen_triangle,
                F32 focal_distance, F32 blur_constant, F32 tan_pixel_angle,
                F32 magnification, F32 max_coc);
}

#endif
