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

    // True when the selected renderer runs on the final linear-HDR image:
    // Advanced (mode 1) or Live (mode 3).
    bool usesScreenSpaceRenderer();

    // Spatially varying lens character of a frame, from the shared lens
    // settings. Focus shifts are in normalized CoC (CoC / max_coc) at the
    // frame corner; the field position is (uv - 0.5) * field scale, length 1
    // at the corner. Used by the Advanced (mode 1) and Live (mode 3)
    // renderers.
    struct LensField
    {
        F32 mFieldScale[2] = { 0.f, 0.f }; // (uv - 0.5) * scale: field position, 1 at the corner
        F32 mCatEye = 0.f;         // barrel shift at the corner, aperture radii; 0 off
        F32 mVignette = 0.f;       // barrel shift whose light loss is kept; 0 off
        F32 mCurvature = 0.f;      // field curvature: signed CoC shift
        F32 mAstigmatism = 0.f;    // radial/circumferential focus split (sign: long axis)
        F32 mAxialCA = 0.f;        // blur shift of the extreme wavelengths
        F32 mSpherical = 0.f;      // spherical aberration, -5..5; 0 off
    };
    // max_coc: the frame's largest blur radius in pixels (> 0).
    LensField lensField(U32 width, U32 height, F32 focal_distance, F32 blur_constant,
                        F32 tan_pixel_angle, F32 magnification, F32 max_coc);

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
