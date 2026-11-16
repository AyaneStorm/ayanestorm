/**
 * @file ashorizonscattering.h
 * @author chanayane@ayanestorm
 * @brief Viewer-local analytic horizon-scattering sky layer.
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

#ifndef AS_HORIZON_SCATTERING_H
#define AS_HORIZON_SCATTERING_H

#include "llglslshader.h"

#include <vector>

namespace ASHorizonScattering
{
    enum BlendMode
    {
        BLEND_ADDITIVE = 0,
        BLEND_ATMOSPHERIC = 1,
        BLEND_REPLACE = 2
    };

    void registerUICallbacks();
    void registerShader(std::vector<LLGLSLShader*>& shaders);
    bool createShader(S32 shader_level);
    void unloadShader();

    // Owns configuration, dome geometry, transform, and blend state. The
    // caller supplies only whether the established sky path selected HDRI.
    void render(bool hdri_sky_active);
    // Returns an AS-owned viewer-cloud shader only while horizon scattering
    // is active; otherwise the caller retains the original EEP cloud shader.
    LLGLSLShader* getCloudShader(bool hdri_sky_active);
    BlendMode getBlendMode();

}

#endif // AS_HORIZON_SCATTERING_H
