/**
 * @file asdiffuseglow.h
 * @author chanayane@ayanestorm
 * @brief Optional bright-surface bloom controls for the existing glow pass.
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

#ifndef AS_DIFFUSE_GLOW_H
#define AS_DIFFUSE_GLOW_H

#include <vector>

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderTarget;
class LLVertexBuffer;

namespace ASDiffuseGlow
{
    void registerUICallbacks();
    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool createHDRShaders(S32 shader_level);
    void unloadHDRShaders();
    void releaseHDRResources();
    void appendShader(LLGLSLShader& shader);
    void bindExtractionUniforms(LLGLSLShader& shader);

    // Returns the scene-linear composite when HDR bloom ran, otherwise null.
    LLRenderTarget* renderHDR(LLRenderTarget& scene, LLRenderTarget& exposure_map,
                              LLVertexBuffer& screen_triangle);
}

#endif
