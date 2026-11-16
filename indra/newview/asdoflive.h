/**
 * @file asdoflive.h
 * @author chanayane@ayanestorm
 * @brief AyaneStorm Live depth of field (ASDepthOfFieldMode 3): layered,
 *        noise-free screen-space renderer.
 *
 * The image is split into four blur bins (strong foreground, foreground,
 * focus, background) kept as premultiplied mip chains at half resolution.
 * Each defocused bin is gathered with area taps (one mip level per tap
 * spacing), so there is no sampling noise at any resolution, and composited
 * front to back over the full-resolution focus bin.
 * See doc/ayanestorm-depth-of-field-live-plan.md.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 * Copyright (C) 2026, Ayane Lyla
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * $/LicenseInfo$
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
