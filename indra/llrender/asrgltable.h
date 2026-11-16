/**
 * @file asrgltable.h
 * @brief With the new renderer on, every GL function pointer comes from here.
 *
 * No GL context exists in that mode. LLGLManager::initGL() still runs unchanged:
 * it asks for each function by name, and this table answers. A few functions
 * reply like a driver, so startup succeeds. All others are stubs that do nothing
 * and log their first call, so a call added by an upstream merge reports itself.
 * New function pointers need no list here, because every name is served.
 *
 * See section 1 of doc/ayanestorm-new-renderer-plan.md.
 * @author chanayane@ayanestorm
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
#ifndef ASR_GLTABLE_H
#define ASR_GLTABLE_H

#include <string>

namespace ASR
{
    // True when the new renderer runs this session. Set once, before the window
    // exists, and never changed.
    bool active();
    void setActive(const std::string& vendor, const std::string& renderer);
}

namespace ASRGLTable
{
    // Never null: unknown names get a stub.
    void* get(const char* name);
}

#endif // ASR_GLTABLE_H
