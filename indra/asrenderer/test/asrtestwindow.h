/**
 * @file asrtestwindow.h
 * asrtestwindow.h
 * The small platform window of asrenderer_test, with one implementation per
 * platform. The viewer has its own windows and does not use this.
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
#pragma once

#include "asrdevice.h"

#include <cstdint>

namespace asrtest
{

class TestWindow
{
public:
    bool create(uint32_t width, uint32_t height, const char* title);

    // Handles pending window events. Returns false once the window is closed.
    bool pumpEvents();

    void resize(uint32_t width, uint32_t height);

    // Current drawable size in pixels.
    void pixelSize(uint32_t& width, uint32_t& height) const;

    asr::SurfaceTarget surfaceTarget() const;

private:
    void* mWindow = nullptr;
    void* mView = nullptr;
};

// True if a GL context is current. The new renderer must never create one. GL
// libraries being loaded proves nothing: drivers and system frameworks load them.
bool glContextIsCurrent();

} // namespace asrtest
