/**
 * @file ascolorgrading.h
 * @author chanayane@ayanestorm
 * @brief Viewer-local photographic color grading and preset storage.
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
#ifndef AS_COLOR_GRADING_H
#define AS_COLOR_GRADING_H

#include <string>
#include <vector>

#include "llglslshader.h"

class LLRenderTarget;
class LLVertexBuffer;

namespace ASColorGrading
{
    // All three rows use perceptual OKLab selection colors.
    enum Band : S32
    {
        RED, ORANGE, YELLOW, GREEN, AQUA, BLUE, PURPLE, MAGENTA,
        GRAY_1, GRAY_2, GRAY_3, GRAY_4, GRAY_5, GRAY_6, GRAY_7, GRAY_8,
        RED_SKIN_2, RED_SKIN_4, RED_SKIN_6, RED_SKIN_8,
        SKIN_2, SKIN_4, SKIN_6, SKIN_8,
        BAND_COUNT
    };

    void registerShaders(std::vector<LLGLSLShader*>& shaders);
    bool createShaders(S32 shader_level);
    void unloadShaders();
    void appendLinearShader(LLGLSLShader& shader);
    void bindLinearUniforms(LLGLSLShader& shader, bool bypass);
    bool present(LLRenderTarget& color, LLRenderTarget& depth, LLVertexBuffer& screen_triangle);
    void refreshStaticGrain();

    bool isActive();
    void setPreviewBypass(bool bypass);
    bool getPreviewBypass();
    void resetAll();
    void resetBand(Band band);
    bool bandModified(Band band);

    const std::vector<std::string>& settingNames();
    std::string bandSettingName(Band band, const std::string& component);
    std::string selectionColorSettingName(Band band);
    std::vector<std::string> listPresets();
    bool isReadOnlyPreset(const std::string& name);
    bool savePreset(const std::string& name);
    bool loadPreset(const std::string& name);
    bool deletePreset(const std::string& name);
}

#endif
