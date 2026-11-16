/**
 * @file asambientocclusion.h
 * @author chanayane@ayanestorm
 * @brief Optional XeGTAO-derived ambient-occlusion backend.
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
#ifndef AS_AMBIENT_OCCLUSION_H
#define AS_AMBIENT_OCCLUSION_H

#include <vector>

#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASAmbientOcclusion
{
    enum Technique
    {
        LEGACY_SSAO = 0,
        GTAO = 1,
        // Visibility bitmask AO: GTAO pipeline with finite-thickness occluders.
        VBAO = 2
    };

    enum Backend
    {
        AUTO = 0,
        FRAGMENT = 1,
        COMPUTE = 2
    };

    const char* shaderCacheRevision();
    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool loadShaders(S32 shader_level);
    void unloadShaders();

    void allocateResources(U32 width, U32 height);
    void releaseResources();

    // Produces GTAO for the current deferred G-buffer. False selects legacy SSAO.
    bool render(LLRenderTarget& deferred_screen, LLVertexBuffer& screen_triangle);

    // Called while the deferred composite shader is bound.
    bool bindResult(LLGLSLShader& shader);
    void unbindResult(LLGLSLShader& shader);

    // Raw visibility after tonemapping, selected by RenderBufferVisualization=7.
    void visualizeResult(LLRenderTarget& destination, LLVertexBuffer& screen_triangle);

    Technique requestedTechnique();
    Technique effectiveTechnique();
    Backend requestedBackend();
    Backend effectiveBackend();
    bool bentNormalsEffective();
    bool debugWhiteEnabled();
}

#endif
