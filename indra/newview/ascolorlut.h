/**
 * @file ascolorlut.h
 * @author chanayane@ayanestorm
 * @brief Optional display-referred .cube LUTs for color grading.
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
#ifndef AS_COLOR_LUT_H
#define AS_COLOR_LUT_H

#include <string>

class LLGLSLShader;
class LLPanel;

namespace ASColorLUT
{
    // Presets store a portable basename, never an arbitrary filesystem path.
    bool validName(const std::string& name);
    bool configureShader(LLGLSLShader& shader);
    void bind(LLGLSLShader& shader);
    void unbind();
    void unload();
    void initPanel(LLPanel& panel);
    void refreshPanel(LLPanel& panel);
    void updatePanel(LLPanel& panel);
}

#endif
