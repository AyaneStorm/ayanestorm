/**
 * @file asmotionblur.h
 * @author chanayane@ayanestorm
 * @brief Screen-space camera motion blur, reprojection-based (no per-object velocity buffer).
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
#ifndef AS_MOTION_BLUR_H
#define AS_MOTION_BLUR_H

#include <vector>
#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASMotionBlur
{
    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();
    // Captures matrices at the end of the 3D scene render, before render_ui() can
    // substitute its HUD modelview. Ordinary snapshots are kept separate and do not
    // advance the module's previous/current live-view history.
    void captureFrameMatrices();
    // Returns true only when a distinct destination received the processed image.
    // depth is the shared deferred depth buffer (e.g. mRT->deferredScreen), read for
    // depth reconstruction and per-sample depth-aware rejection.
    bool render(LLRenderTarget& source, LLRenderTarget& destination, LLRenderTarget& depth,
                LLVertexBuffer& screen_triangle);
}
#endif
