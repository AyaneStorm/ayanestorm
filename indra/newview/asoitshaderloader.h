/**
 * @file asoitshaderloader.h
 * @brief AyaneStorm frame-budgeted loading of the OIT renderers' shaders.
 * @author chanayane@ayanestorm
 *
 * A viewer-wide shader reload used to compile every OIT family (Exact OIT,
 * AVBOIT, Mac OIT) inline, although at most two are in use. On a shader cache
 * miss that blocked the main loop ~13 s per reload. Only the families the
 * current settings need are now compiled, a few programs per frame, while
 * transparency falls back to the vanilla path until a family is complete.
 * See doc/ayanestorm-graphics-switch-shader-reload-stall.md.
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

#ifndef AS_OIT_SHADER_LOADER_H
#define AS_OIT_SHADER_LOADER_H

#include "stdtypes.h"

class ASOITShaderLoader
{
public:
    // Called by LLViewerShaderMgr::loadShadersDeferred() in place of compiling
    // the OIT families inline: records the compile parameters and forgets every
    // family, since the viewer-wide reload has just unloaded their programs.
    static void reset(S32 shader_level, bool use_sun_shadow, bool gltf_enabled);
    // Called by LLViewerShaderMgr::unloadShaders(): every program is gone, so
    // nothing may compile until the next reset() (a failed reload never sends one).
    static void invalidate();
    // Once per frame, outside rendering: queues the families the current
    // settings need (a later mode change queues its family here too), cancels
    // the ones no longer needed, and compiles queued programs within a small
    // time budget.
    static void update();
    // True while any family is still compiling.
    static bool loading();
};

#endif
