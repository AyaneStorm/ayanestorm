/**
 * @file aswaterhorizonfog.h
 * @author chanayane@ayanestorm
 * @brief AyaneStorm water-horizon fog uniform setup.
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

#ifndef AS_WATER_HORIZON_FOG_H
#define AS_WATER_HORIZON_FOG_H

class LLGLSLShader;

namespace ASWaterHorizonFog
{
    // Upload viewer-local fog controls while the water shader is bound.
    void uploadUniforms(LLGLSLShader& shader);
}

#endif
