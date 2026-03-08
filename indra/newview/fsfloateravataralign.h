/**
 * @file fsfloateravataralign.h
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

#ifndef FS_FLOATER_AVATAR_ALIGN_H
#define FS_FLOATER_AVATAR_ALIGN_H

#include "llfloater.h"

class FSFloaterAvatarAlign : public LLFloater
{
    LOG_CLASS(FSFloaterAvatarAlign);
    friend class LLFloaterReg;
private:
    FSFloaterAvatarAlign(const LLSD& key);
    ~FSFloaterAvatarAlign() override;
public:
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void onClickCardinal(F32 target_deg);
    void onClickNearest();
    void onClickFaceNearestAvatar();
    void rotateAgentTo(F32 target_deg);
    void applyRotation(const LLVector3& direction);
    void snapAvatarBody(const LLVector3& target_at);
    LLVector3 offsetDirection(const LLVector3& base_at, F32 offset_deg) const;

    // Oscillation sequence: send offset rotations via AgentUpdate so remote
    // viewers' pelvis-lag animation commits to the target direction.
    // Locally, mRoot is always overridden to the final target (no visible jitter).
    struct OscStep { F32 offset_deg; F32 hold_sec; };
    static const OscStep OSCILLATION[];

    LLVector3 mTargetDirection;   // final desired facing (horizontal, normalised)
    S32       mOscStep = -1;      // current index into OSCILLATION, -1 = idle
    LLTimer   mOscTimer;
};

#endif // FS_FLOATER_AVATAR_ALIGN_H
