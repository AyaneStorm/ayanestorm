/**
 * @file asshaderreload.h
 * @brief AyaneStorm: one viewer-wide shader reload per frame at most.
 * @author chanayane@ayanestorm
 *
 * Every setting with a shader listener calls LLViewerShaderMgr::setShaders()
 * itself. Code that writes several such settings in a row (graphics level,
 * UI refresh, presets, tuning) reloaded once per setting, each time with a
 * new intermediate set of global defines, so each reload missed the shader
 * cache and compiled every program (7-9 s each on the test machine).
 * While shaders are loaded, requests are now recorded and served by a single
 * reload at the start of the next displayed frame, with the final settings.
 * The current programs stay valid until then. See
 * doc/ayanestorm-graphics-switch-shader-reload-stall.md.
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

#ifndef AS_SHADER_RELOAD_H
#define AS_SHADER_RELOAD_H

namespace ASShaderReload
{
    // Called first in LLViewerShaderMgr::setShaders(): true when the request
    // was recorded for flush(). Never defers while no shaders are loaded
    // (startup, GL restore), since callers then need them immediately.
    bool defer();
    // Called by display() before any frame work: runs the recorded reload.
    void flush();
}

#endif
