/**
 * @file aschromaticaberration.h
 * @author chanayane@ayanestorm
 * @brief Optional radial camera chromatic aberration.
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
#ifndef AS_CHROMATIC_ABERRATION_H
#define AS_CHROMATIC_ABERRATION_H

#include <vector>
#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASChromaticAberration
{
    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();
    // Returns true only when a distinct destination received the processed image.
    bool render(LLRenderTarget& source, LLRenderTarget& destination, LLVertexBuffer& screen_triangle);
    // Draws a screen-space crosshair at the configured center when the beacon is enabled.
    void renderCenterBeacon();
}
#endif
