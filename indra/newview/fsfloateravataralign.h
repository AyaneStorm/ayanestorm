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

class LLVOAvatar;

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
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;

    // Face the nearest non-flying avatar within MAX_FACE_DISTANCE.
    void onClickFaceNearestAvatar();

    // Face a specific avatar. Safe to call with nullptr (no-op).
    // Callable from external contexts (minimap context menu, etc.).
    void faceAvatar(LLVOAvatar* avatar);

    // Returns true if avatar is non-null, alive, and within MAX_FACE_DISTANCE metres.
    bool isAvatarInRange(LLVOAvatar* avatar) const;

    static constexpr F32 MAX_FACE_DISTANCE = 20.f;

private:
    void onClickCardinal(F32 target_deg);
    void onClickRotate(F32 delta_deg);
    void onClickNearest();
    void rotateAgentTo(F32 target_deg);
    void applyRotation(const LLVector3& direction);
    void snapAvatarBody(const LLVector3& target_at);
    LLVector3 offsetDirection(const LLVector3& base_at, F32 offset_deg) const;
    void drawCompass();

    // Oscillation sequence: send offset rotations via AgentUpdate so remote
    // viewers' pelvis-lag animation commits to the target direction.
    // Locally, mRoot is always overridden to the final target (no visible jitter).
    struct OscStep { F32 offset_deg; F32 hold_sec; };
    static const OscStep OSCILLATION[];

    void snapRemoteAvatarBody(LLVOAvatar* avatar);

    LLVector3  mTargetDirection;            // final desired facing (horizontal, normalised)
    S32        mOscStep = -1;               // current index into OSCILLATION, -1 = idle
    LLTimer    mOscTimer;
    LLVOAvatar* mTargetAvatar = nullptr;   // remote avatar to snap during oscillation

    // Compass geometry, computed each draw() and used by handleMouseDown/handleHover().
    S32 mCompassCX = 0;
    S32 mCompassCY = 0;
    S32 mCompassR  = 0;
    // Hover state: -1=none, -2=centre, 0-7=octant (index × 45° = degrees from North CW)
    S32 mHoverOctant = -1;
};

#endif // FS_FLOATER_AVATAR_ALIGN_H
