/**
 * @file asdoflive.h
 * @author chanayane@firestorm
 * @brief AyaneStorm Live depth of field (ASDepthOfFieldMode 3): layered,
 *        noise-free screen-space renderer.
 *
 * The image is split into four blur bins (strong foreground, foreground,
 * focus, background) kept as premultiplied mip chains at half resolution.
 * Each defocused bin is gathered with area taps (one mip level per tap
 * spacing), so there is no sampling noise at any resolution, and composited
 * front to back over the full-resolution focus bin.
 * See doc/ayanestorm-depth-of-field-live-plan.md.
 */
#ifndef AS_DOF_LIVE_H
#define AS_DOF_LIVE_H

#include <vector>

#include "asmacoit.h"
#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASDoFLive
{
    // ASDepthOfFieldMode value of this renderer.
    constexpr S32 LIVE_MODE = 3;

    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool createShaders(S32 shader_level);
    void unloadShaders();
    void releaseResources();

    // Transparency bins (plan phase 2). The capture runs before this frame's
    // focus is known, so it uses the last Live frame's lens, rescaled to the
    // capture's image size. False when Live DoF is not active or has not
    // rendered yet; the frame then uses the one-layer fallback.
    bool transparencyLens(U32 width, U32 height, ASMacOIT::DoFLens& lens);
    // Called before the post-water alpha pool renders: keeps the opaque
    // colour and depth the bins are completed with.
    void prepareCapture(U32 width, U32 height);

    // Same contract as ASDepthOfField::render(): true only after destination
    // was written. Parameters are Firestorm's lens frontend values
    // (pipeline.cpp renderDoF()).
    bool render(LLRenderTarget& source, LLRenderTarget& destination,
                LLRenderTarget& depth, LLVertexBuffer& screen_triangle,
                F32 focal_distance, F32 blur_constant, F32 tan_pixel_angle,
                F32 magnification, F32 max_coc);
}

#endif
