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

    // Dedicated linear-HDR output; never alias the final scene or its depth.
    LLRenderTarget* hdrOutput(U32 width, U32 height);

    // Captures transparent visibility independently from opaque scene depth.
    // Coverage accumulates every contributing fragment; depth remains nearest.
    bool prepareTransparentDepthCapture(U32 width, U32 height);
    bool beginTransparentCoverageCapture(U32 width, U32 height);
    bool snapshotRiggedCoverage();
    void endTransparentCoverageCapture();
    bool beginTransparentDepthCapture(U32 width, U32 height);
    bool snapshotRiggedDepth();
    void endTransparentDepthCapture();
    bool beginWorldDepthCapture(U32 width, U32 height);
    void endWorldDepthCapture();

    // Returns true only after the advanced renderer has written destination.
    // The caller must continue through the legacy path when this returns false.
    bool render(LLRenderTarget& source, LLRenderTarget& destination,
                LLRenderTarget& depth, LLVertexBuffer& screen_triangle,
                F32 focal_distance, F32 blur_constant, F32 tan_pixel_angle,
                F32 magnification, F32 max_coc);
}

#endif
