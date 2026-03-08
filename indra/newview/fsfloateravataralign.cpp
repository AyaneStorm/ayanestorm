/**
 * @file fsfloateravataralign.cpp
 * @brief Floater for rotating the avatar to face cardinal directions or nearest avatar
 *
 * $LicenseInfo:firstyear=2025&license=viewerlgpl$
 * AyaneStorm Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsfloateravataralign.h"

#include "llagent.h"
#include "llviewercontrol.h"
#include "llcharacter.h"
#include "llfloaterreg.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llviewermessage.h"
#include "llanimationstates.h"

// Oscillation sequence sent via AgentUpdate to force remote viewers' pelvis-lag
// animation to commit to the target direction.  Each step is an angular offset
// from the target, held long enough (~one PELVIS_LAG_WALKING period = 0.4 s)
// for the remote avatar's pelvis to actually move before the next correction.
// The final entry (offset 0) is the true target and is held until the sequence ends.
// Locally mRoot is always overridden to the final target — no visible jitter.
const FSFloaterAvatarAlign::OscStep FSFloaterAvatarAlign::OSCILLATION[] =
{
    { 45.f, 0.4f },
    {-45.f, 0.4f },
    { 20.f, 0.35f},
    {-20.f, 0.35f},
    {  8.f, 0.3f },
    { -8.f, 0.3f },
    {  0.f, 0.3f },   // final: correct direction
};

namespace
{

bool isAvatarFlying(LLVOAvatar* avatar)
{
    if (!avatar) return false;

    const LLVOAvatar::AnimIterator end = avatar->mSignaledAnimations.end();
    for (LLVOAvatar::AnimIterator it = avatar->mSignaledAnimations.begin(); it != end; ++it)
    {
        if (it->first == ANIM_AGENT_FLY        ||
            it->first == ANIM_AGENT_HOVER      ||
            it->first == ANIM_AGENT_HOVER_DOWN ||
            it->first == ANIM_AGENT_HOVER_UP)
        {
            return true;
        }
    }
    return false;
}

} // namespace

FSFloaterAvatarAlign::FSFloaterAvatarAlign(const LLSD& key)
    : LLFloater(key)
{
}

FSFloaterAvatarAlign::~FSFloaterAvatarAlign()
{
}

bool FSFloaterAvatarAlign::postBuild()
{
    childSetAction("btn_north",   [this](void*) { onClickCardinal(0.f);       }, this);
    childSetAction("btn_east",    [this](void*) { onClickCardinal(90.f);      }, this);
    childSetAction("btn_south",   [this](void*) { onClickCardinal(180.f);     }, this);
    childSetAction("btn_west",    [this](void*) { onClickCardinal(270.f);     }, this);
    childSetAction("btn_nearest", [this](void*) { onClickNearest();           }, this);
    childSetAction("btn_avatar",  [this](void*) { onClickFaceNearestAvatar(); }, this);

    return true;
}

void FSFloaterAvatarAlign::onOpen(const LLSD& key)
{
}

void FSFloaterAvatarAlign::draw()
{
    LLFloater::draw();

    if (mOscStep < 0)
        return;

    // Always override mRoot to the final target so local view looks correct.
    snapAvatarBody(mTargetDirection);

    S32 last = (S32)(LL_ARRAY_SIZE(OSCILLATION)) - 1;
    if (mOscTimer.getElapsedTimeF32() >= OSCILLATION[mOscStep].hold_sec)
    {
        ++mOscStep;
        if (mOscStep > last)
        {
            // Sequence done — ensure final correct rotation is applied.
            gAgent.resetAxes(mTargetDirection);
            send_agent_update(true, false);
            mOscStep = -1;
            return;
        }
        LLVector3 stepped = offsetDirection(mTargetDirection, OSCILLATION[mOscStep].offset_deg);
        gAgent.resetAxes(stepped);
        send_agent_update(true, false);
        mOscTimer.reset();
    }
}

// Rotate at by offset_deg around the world Z axis.
LLVector3 FSFloaterAvatarAlign::offsetDirection(const LLVector3& base_at, F32 offset_deg) const
{
    F32 rad = offset_deg * DEG_TO_RAD;
    F32 c = cosf(rad), s = sinf(rad);
    return LLVector3(base_at.mV[VX] * c - base_at.mV[VY] * s,
                     base_at.mV[VX] * s + base_at.mV[VY] * c,
                     0.f);
}

// Override mRoot world rotation to match target_at for this frame.
// Replicates the fwdDir/wQv math from LLVOAvatar::updateCharacter() so that
// pelvisDir equals fwdDir, zeroing the correction vector locally.
void FSFloaterAvatarAlign::snapAvatarBody(const LLVector3& target_at)
{
    if (!isAgentAvatarValid() || !gAgentAvatarp->mRoot)
        return;

    LLVector3 at = target_at;
    at.mV[VZ] = 0.f;
    if (at.normalize() < 0.001f)
        return;

    LLVector3 up(0.f, 0.f, 1.f);
    LLVector3 left = up % at;
    left.normalize();
    at = left % up;

    gAgentAvatarp->mRoot->setWorldRotation(LLQuaternion(at, left, up));
}

// Begin the oscillation sequence (or a direct snap) toward direction.
void FSFloaterAvatarAlign::applyRotation(const LLVector3& direction)
{
    mTargetDirection = direction;

    if (gSavedSettings.getBOOL("AvatarAlignOscillate"))
    {
        // Oscillate: remote viewers receive offset AgentUpdates so their
        // pelvis-lag animation commits to the target direction.
        mOscStep = 0;
        LLVector3 stepped = offsetDirection(mTargetDirection, OSCILLATION[0].offset_deg);
        gAgent.resetAxes(stepped);
        send_agent_update(true, false);
        mOscTimer.reset();
    }
    else
    {
        // Direct snap: apply the correct rotation immediately.
        mOscStep = -1;
        gAgent.resetAxes(mTargetDirection);
        send_agent_update(true, false);
        snapAvatarBody(mTargetDirection);
    }
}

// Rotate agent to face a world direction given in degrees.
// Convention: 0=North (+Y), 90=East (+X), 180=South, 270=West.
void FSFloaterAvatarAlign::rotateAgentTo(F32 target_deg)
{
    F32 yaw_rad = target_deg * DEG_TO_RAD;
    // In SL coords: X = East, Y = North
    LLVector3 look_at(sinf(yaw_rad), cosf(yaw_rad), 0.f);
    applyRotation(look_at);
}

void FSFloaterAvatarAlign::onClickCardinal(F32 target_deg)
{
    rotateAgentTo(target_deg);
}

void FSFloaterAvatarAlign::onClickNearest()
{
    // Read the current forward (at) axis from mFrameAgent and flatten to horizontal.
    LLVector3 at = gAgent.getFrameAgent().getAtAxis();
    at.mV[VZ] = 0.f;
    at.normalize();

    // atan2 in SL coords (X=East, Y=North) gives world yaw.
    F32 yaw_deg = atan2f(at.mV[VX], at.mV[VY]) * RAD_TO_DEG;
    yaw_deg = fmodf(yaw_deg + 360.f, 360.f);

    // Round to nearest 45°
    F32 nearest_deg = fmodf((F32)(ll_round(yaw_deg / 45.f) * 45), 360.f);
    rotateAgentTo(nearest_deg);
}

void FSFloaterAvatarAlign::onClickFaceNearestAvatar()
{
    if (!isAgentAvatarValid())
        return;

    LLVector3 my_pos = gAgent.getPositionAgent();
    LLVOAvatar* nearest = nullptr;
    F32 nearest_dist_sq = F32_MAX;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (avatar->isDead() || avatar->isControlAvatar() || avatar->isSelf())
            continue;
        if (isAvatarFlying(avatar))
            continue;

        F32 dist_sq = dist_vec_squared(avatar->getPositionAgent(), my_pos);
        if (dist_sq < nearest_dist_sq)
        {
            nearest_dist_sq = dist_sq;
            nearest = avatar;
        }
    }

    if (!nearest)
    {
        LL_WARNS("AvatarAlign") << "No nearby avatar found to face." << LL_ENDL;
        return;
    }

    LLVector3 direction = nearest->getPositionAgent() - my_pos;
    direction.mV[VZ] = 0.f;
    direction.normalize();

    applyRotation(direction);
}
