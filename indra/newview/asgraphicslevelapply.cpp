/**
 * @file asgraphicslevelapply.cpp
 * @brief AyaneStorm: apply a graphics quality change once the slider settles.
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

#include "llviewerprecompiledheaders.h"

#include "asgraphicslevelapply.h"

#include "llcallbacklist.h"
#include "llfocusmgr.h"
#include "llframetimer.h"

namespace
{
// Quiet time after the last change, which also merges keyboard steps.
constexpr F32 SETTLE_SECONDS = 0.3f;

std::function<void()> sPending;
LLFrameTimer sSinceRequest;
bool sIdleRegistered = false;
bool sApplying = false;

bool onIdle()
{
    // A drag holds the mouse capture until release.
    if (gFocusMgr.getMouseCapture() || sSinceRequest.getElapsedTimeF32() < SETTLE_SECONDS)
    {
        return false;
    }
    std::function<void()> apply;
    apply.swap(sPending);
    sIdleRegistered = false;
    sApplying = true;
    apply();
    sApplying = false;
    return true;
}
}

namespace ASGraphicsLevelApply
{
void request(std::function<void()> apply)
{
    sPending = std::move(apply);
    sSinceRequest.reset();
    if (!sIdleRegistered)
    {
        sIdleRegistered = true;
        doOnIdleRepeating(onIdle);
    }
}

bool applying()
{
    return sApplying;
}
}
