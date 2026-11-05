/**
 * @file asgraphicslevelapply.cpp
 * @brief AyaneStorm: apply a graphics quality change once the slider settles.
 * @author chanayane@firestorm
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
