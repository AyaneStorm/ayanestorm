/**
 * @file aslensflare.h
 * @author chanayane@ayanestorm
 * @brief Viewer-local screen-space sun and moon lens flares.
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
