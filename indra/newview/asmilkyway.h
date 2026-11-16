/**
 * @file asmilkyway.h
 * @author chanayane@ayanestorm
 * @brief Viewer-local real-sky Milky Way and deep-sky glow, drawn on the sky
 * dome from a catalogue-derived equirectangular texture.
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

#ifndef AS_MILKY_WAY_H
#define AS_MILKY_WAY_H

#include <vector>

#include "llglslshader.h"

class LLViewerFetchedTexture;

namespace ASMilkyWay
{
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    // Also releases the GL texture (shader reload / GL restore).
    void unloadShader();
    // Binds the program and texture and sets uniforms; false when the glow
    // must not be drawn (stars off, procedural mode, disabled, HDRI sky,
    // daylight, missing texture).
    bool configureShader(bool hdri_sky);
    LLGLSLShader& getShader();

    // The Milky Way and deep sky levels (night factor included) and saturation, for GL and
    // the new renderer alike; false when no glow shows.
    bool levels(F32& band, F32& deep_sky, F32& saturation);
    // The new renderer's copy of a map, through the texture list (loaded on first use).
    LLViewerFetchedTexture* rendererMap(bool deep_sky);
}

#endif
