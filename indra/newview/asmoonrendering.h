/**
 * @file asmoonrendering.h
 * @author chanayane@ayanestorm
 * @brief Viewer-local moon disc and halo rendering integration.
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

#ifndef AS_MOON_RENDERING_H
#define AS_MOON_RENDERING_H

#include "v3color.h"

class LLFace;
class LLGLSLShader;
class LLSettingsSky;

namespace ASMoonRendering
{
    // Registers reset buttons used by the standalone settings panel.
    void registerUICallbacks();

    void renderHalo(LLFace* halo_face, LLFace* moon_face, LLGLSLShader* shader);
    void configureDiscShader(LLGLSLShader* shader, const LLSettingsSky* sky);

    // The horizon colour (ASMoonHorizonTint*) for a moon at this height (its direction's z):
    // white high up, towards the tint near the horizon. As moonF.glsl computes it for the disc.
    LLColor3 horizonTint(F32 moon_z);
    // ASMoonSizeMultiplier, on top of the sky's moon scale.
    F32 sizeMultiplier();
    // The moon's angular radius as OpenGL draws it, scale and multiplier included, radians.
    F32 angularRadius(const LLSettingsSky& sky);

    // Alt-click on the moon (lltoolfocus.cpp, where a pick hits only sky): true when the
    // click at this window point was on the moon, and the camera now turns to it.
    bool focusOnMoon(S32 x, S32 y);
    // The scroll wheel while the camera is on the moon zooms the field of view, as a
    // telescope does, instead of moving the camera (llagentcamera.cpp). True when used.
    bool handleScrollWheel(S32 clicks);
}

#endif
