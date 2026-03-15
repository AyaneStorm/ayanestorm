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
#include "llfontgl.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llviewermessage.h"
#include "llanimationstates.h"

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

// ============================================================
// FSAvatarAlignBase
// ============================================================

FSAvatarAlignBase::FSAvatarAlignBase(const LLSD& key)
    : LLFloater(key)
{
}

// static
FSAvatarAlignBase* FSAvatarAlignBase::getActive()
{
    if (gSavedSettings.getBOOL("AvatarAlignMini"))
        return LLFloaterReg::getTypedInstance<FSFloaterAvatarAlignMini>("avatar_align_mini");
    return LLFloaterReg::getTypedInstance<FSFloaterAvatarAlign>("avatar_align");
}

void FSAvatarAlignBase::draw()
{
    LLFloater::draw();
    drawCompass();
}

void FSAvatarAlignBase::drawCompass()
{
    LLRect local     = getLocalRect();
    S32    header_h  = getHeaderHeight();

    S32 area_top    = local.mTop    - header_h - getToolbarHeight();
    S32 area_bottom = local.mBottom + getBottomReserve();
    S32 avail_h     = area_top - area_bottom;
    S32 avail_w     = local.getWidth();

    // Vertical clearances: top_clear + bot_clear = overhead.
    //   Full:       27 (N label) + 53 (S label + bearing gap + margin) = 80
    //   Mini:        7 (toolbar gap) + 22 (bearing gap + text) = 29
    // R is always computed with the same overhead so it never jumps discontinuously.
    // mini_compact is a pure drawing decision derived from the resulting R.
    S32 top_clear = isMiniMode() ?  7 : 27;
    S32 overhead  = isMiniMode() ? 29 : 80;
    // Full mode needs extra horizontal margin so E/W labels don't clip the floater edge
    S32 h_margin = isMiniMode() ? 8 : 28;
    S32 R = llmax(llmin(avail_w / 2 - h_margin, (avail_h - overhead) / 2), 30);
    // Hide bearing and intercardinals when mini compass is too small to be legible
    bool mini_compact = isMiniMode() && R < 50;

    mCompassCX = local.getCenterX();
    mCompassCY = area_top - top_clear - R;
    mCompassR  = R;

    F32 cx = (F32)mCompassCX;
    F32 cy = (F32)mCompassCY;
    F32 fR = (F32)R;

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // Background circle
    gGL.color4f(0.08f, 0.08f, 0.10f, 0.90f);
    gl_circle_2d(cx, cy, fR, 64, TRUE);

    // Outer ring
    gGL.color4f(0.45f, 0.45f, 0.45f, 1.f);
    gl_circle_2d(cx, cy, fR, 64, FALSE);

    // Inner ring at half radius
    gGL.color4f(0.25f, 0.25f, 0.25f, 1.f);
    gl_circle_2d(cx, cy, fR * 0.5f, 48, FALSE);

    // Tick marks at 45° intervals
    gGL.begin(LLRender::LINES);
    gGL.color4f(0.35f, 0.35f, 0.35f, 1.f);
    for (S32 i = 0; i < 8; ++i)
    {
        F32 a  = i * F_PI / 4.f;
        F32 sa = sinf(a), ca = cosf(a);
        gGL.vertex2f(cx + (fR - 7.f) * sa, cy + (fR - 7.f) * ca);
        gGL.vertex2f(cx +  fR        * sa, cy +  fR        * ca);
    }
    gGL.end();

    // Hover highlight
    if (mHoverOctant == -2)
    {
        gGL.color4f(1.f, 1.f, 1.f, 0.18f);
        gl_circle_2d(cx, cy, fR * 0.25f, 24, TRUE);
    }
    else if (mHoverOctant >= 0)
    {
        F32 hoverAngle = mHoverOctant * 45.f * DEG_TO_RAD;
        F32 halfSpan   = F_PI / 8.f;
        F32 innerR     = fR * 0.25f;
        S32 segs       = 8;
        gGL.begin(LLRender::TRIANGLES);
        gGL.color4f(1.f, 1.f, 1.f, 0.13f);
        for (S32 i = 0; i < segs; ++i)
        {
            F32 a0 = hoverAngle - halfSpan + (F32)i       * (2.f * halfSpan / segs);
            F32 a1 = hoverAngle - halfSpan + (F32)(i + 1) * (2.f * halfSpan / segs);
            gGL.vertex2f(cx + innerR * sinf(a0), cy + innerR * cosf(a0));
            gGL.vertex2f(cx + fR     * sinf(a0), cy + fR     * cosf(a0));
            gGL.vertex2f(cx + fR     * sinf(a1), cy + fR     * cosf(a1));
            gGL.vertex2f(cx + innerR * sinf(a0), cy + innerR * cosf(a0));
            gGL.vertex2f(cx + fR     * sinf(a1), cy + fR     * cosf(a1));
            gGL.vertex2f(cx + innerR * sinf(a1), cy + innerR * cosf(a1));
        }
        gGL.end();
    }

    // 4 cardinal arm kite shapes
    struct ArmDef { F32 angle_deg; F32 r, g, b; };
    static const ArmDef ARMS[] = {
        {   0.f, 0.85f, 0.15f, 0.15f },  // North: red
        { 180.f, 0.85f, 0.85f, 0.85f },  // South: white
        {  90.f, 0.65f, 0.65f, 0.65f },  // East:  grey
        { 270.f, 0.65f, 0.65f, 0.65f },  // West:  grey
    };

    F32 tipR  = fR - 4.f;
    F32 sideR = fR * 0.245f;

    gGL.begin(LLRender::TRIANGLES);
    for (const auto& arm : ARMS)
    {
        F32 a  = arm.angle_deg * DEG_TO_RAD;
        F32 al = a - F_PI_BY_TWO;
        F32 ar = a + F_PI_BY_TWO;

        F32 tx = cx + tipR  * sinf(a);   F32 ty = cy + tipR  * cosf(a);
        F32 lx = cx + sideR * sinf(al);  F32 ly = cy + sideR * cosf(al);
        F32 rx = cx + sideR * sinf(ar);  F32 ry = cy + sideR * cosf(ar);

        gGL.color4f(arm.r, arm.g, arm.b, 1.f);
        gGL.vertex2f(tx, ty); gGL.vertex2f(cx, cy); gGL.vertex2f(lx, ly);
        gGL.vertex2f(tx, ty); gGL.vertex2f(rx, ry); gGL.vertex2f(cx, cy);
    }
    gGL.end();

    // Intercardinal arms: NE, SE, SW, NW (hidden in mini mode below 150px wide)
    F32 tipR2  = fR * 0.62f;
    F32 sideR2 = fR * 0.10f;
    static const F32 INTER_ANGLES[] = { 45.f, 135.f, 225.f, 315.f };

    if (!mini_compact)
    {
        gGL.begin(LLRender::TRIANGLES);
        gGL.color4f(0.55f, 0.55f, 0.55f, 1.f);
        for (F32 angle_deg : INTER_ANGLES)
        {
            F32 a  = angle_deg * DEG_TO_RAD;
            F32 al = a - F_PI_BY_TWO;
            F32 ar = a + F_PI_BY_TWO;

            F32 tx = cx + tipR2  * sinf(a);   F32 ty = cy + tipR2  * cosf(a);
            F32 lx = cx + sideR2 * sinf(al);  F32 ly = cy + sideR2 * cosf(al);
            F32 rx = cx + sideR2 * sinf(ar);  F32 ry = cy + sideR2 * cosf(ar);

            gGL.vertex2f(tx, ty); gGL.vertex2f(cx, cy); gGL.vertex2f(lx, ly);
            gGL.vertex2f(tx, ty); gGL.vertex2f(rx, ry); gGL.vertex2f(cx, cy);
        }
        gGL.end();
    } // end intercardinal guard

    // Center dot
    gGL.color4f(0.20f, 0.20f, 0.20f, 1.f);
    gl_circle_2d(cx, cy, 5.f, 16, TRUE);
    gGL.color4f(0.50f, 0.50f, 0.50f, 1.f);
    gl_circle_2d(cx, cy, 5.f, 16, FALSE);

    // Heading needle
    LLVector3 at = gAgent.getAtAxis();
    at.mV[VZ] = 0.f;
    if (at.normalize() > 0.01f)
    {
        F32 nl  = tipR * 0.88f;
        F32 pw  = 4.f;
        F32 nx  = cx + nl * at.mV[VX];
        F32 ny  = cy + nl * at.mV[VY];
        F32 bx1 = cx - at.mV[VY] * pw;  F32 by1 = cy + at.mV[VX] * pw;
        F32 bx2 = cx + at.mV[VY] * pw;  F32 by2 = cy - at.mV[VX] * pw;

        gGL.begin(LLRender::TRIANGLES);
        gGL.color4f(1.f, 0.85f, 0.f, 0.95f);
        gGL.vertex2f(nx, ny); gGL.vertex2f(bx1, by1); gGL.vertex2f(bx2, by2);
        gGL.end();
    }

    // Cardinal labels: only in full mode and when compass is large enough
    LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (!isMiniMode() && R >= 50)
    {
        S32      ld    = R + 10;
        LLColor4 col_n(1.f, 0.55f, 0.55f, 1.f);
        LLColor4 col_o(0.90f, 0.90f, 0.90f, 1.f);

        font->renderUTF8("N", 0, cx,                    (F32)(mCompassCY + ld), col_n, LLFontGL::HCENTER, LLFontGL::BOTTOM,  LLFontGL::BOLD,   LLFontGL::DROP_SHADOW);
        font->renderUTF8("S", 0, cx,                    (F32)(mCompassCY - ld), col_o, LLFontGL::HCENTER, LLFontGL::TOP,     LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        font->renderUTF8("E", 0, (F32)(mCompassCX + ld),(F32)mCompassCY,        col_o, LLFontGL::LEFT,    LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        font->renderUTF8("W", 0, (F32)(mCompassCX - ld),(F32)mCompassCY,        col_o, LLFontGL::RIGHT,   LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }

    // Bearing label: hidden in mini compact mode to give compass more room
    if (!mini_compact)
    {
        LLVector3 hat = gAgent.getAtAxis();
        hat.mV[VZ] = 0.f;
        hat.normalize();
        F32 bearing = fmodf(atan2f(hat.mV[VX], hat.mV[VY]) * RAD_TO_DEG + 360.f, 360.f);
        std::string bearing_str = llformat("%03.0f\xC2\xB0", bearing);
        // Mini: bearing tight below ring; full: more clearance to avoid S label overlap
        S32 bearing_y = isMiniMode() ? (mCompassCY - R - 10) : (mCompassCY - R - 25);
        font->renderUTF8(bearing_str, 0, cx, (F32)bearing_y,
            LLColor4(0.85f, 0.85f, 0.85f, 1.f), LLFontGL::HCENTER, LLFontGL::TOP,
            LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }

    // Toggle-mode button: top-right corner of compass bounding square
    const std::string lbl = isMiniMode() ? "Mini" : "Full";
    S32 btn_w   = (S32)font->getWidth(lbl) + 8;
    S32 btn_h   = 14;
    // Mini: top-right of ring bounding square; Full: top-right of square including N/E labels
    S32 sq_half = isMiniMode() ? R : (R + 20);
    S32 btn_x   = mCompassCX + sq_half - btn_w;
    S32 btn_yt  = mCompassCY + sq_half;

    mToggleBtnRect.set(btn_x, btn_yt, btn_x + btn_w, btn_yt - btn_h);

    gGL.color4f(0.f, 0.f, 0.f, 0.55f);
    gl_rect_2d(mToggleBtnRect, true);
    gGL.color4f(1.f, 1.f, 1.f, mHoverToggle ? 1.f : 0.65f);
    gl_rect_2d(mToggleBtnRect, false);

    font->renderUTF8(lbl, 0,
        (F32)(mToggleBtnRect.mLeft + mToggleBtnRect.mRight) * 0.5f,
        (F32)(mToggleBtnRect.mBottom + mToggleBtnRect.mTop) * 0.5f,
        LLColor4::white, LLFontGL::HCENTER, LLFontGL::VCENTER,
        LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
}

bool FSAvatarAlignBase::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // Toggle button takes priority
    if (mToggleBtnRect.notEmpty() && mToggleBtnRect.pointInRect(x, y))
    {
        onToggleMode();
        return true;
    }

    if (mCompassR > 0)
    {
        S32 dx   = x - mCompassCX;
        S32 dy   = y - mCompassCY;
        F32 dist = sqrtf((F32)(dx * dx + dy * dy));

        if (dist <= (F32)mCompassR)
        {
            if (dist < (F32)mCompassR * 0.25f)
            {
                onClickFaceNearestAvatar();
            }
            else
            {
                F32 deg    = atan2f((F32)dx, (F32)dy) * RAD_TO_DEG;
                deg        = fmodf(deg + 360.f, 360.f);
                F32 octant = fmodf((F32)(ll_round(deg / 45.f)) * 45.f, 360.f);
                onClickCardinal(octant);
            }
            return true;
        }
    }
    return LLFloater::handleMouseDown(x, y, mask);
}

bool FSAvatarAlignBase::handleHover(S32 x, S32 y, MASK mask)
{
    mHoverToggle = mToggleBtnRect.notEmpty() && mToggleBtnRect.pointInRect(x, y);
    mHoverOctant = -1;

    if (!mHoverToggle && mCompassR > 0)
    {
        S32 dx   = x - mCompassCX;
        S32 dy   = y - mCompassCY;
        F32 dist = sqrtf((F32)(dx * dx + dy * dy));
        if (dist <= (F32)mCompassR)
        {
            if (dist < (F32)mCompassR * 0.25f)
                mHoverOctant = -2;
            else
            {
                F32 deg = fmodf(atan2f((F32)dx, (F32)dy) * RAD_TO_DEG + 360.f, 360.f);
                mHoverOctant = (S32)(ll_round(deg / 45.f)) % 8;
            }
        }
    }
    return LLFloater::handleHover(x, y, mask);
}

void FSAvatarAlignBase::snapAvatarBody(const LLVector3& target_at)
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
    gAgentAvatarp->mRoot->setWorldPosition(gAgent.getPositionAgent());
}

void FSAvatarAlignBase::snapRemoteAvatarBody(LLVOAvatar* avatar)
{
    if (!avatar || avatar->isDead() || !avatar->mRoot)
        return;

    LLVector3 at = LLVector3(1.f, 0.f, 0.f) * avatar->getRotation();
    at.mV[VZ] = 0.f;
    if (at.normalize() < 0.001f)
        return;

    LLVector3 up(0.f, 0.f, 1.f);
    LLVector3 left = up % at;
    left.normalize();
    at = left % up;

    avatar->mRoot->setWorldRotation(LLQuaternion(at, left, up));
    avatar->mRoot->setWorldPosition(avatar->getPositionAgent());
}

void FSAvatarAlignBase::applyRotation(const LLVector3& direction)
{
    gAgent.resetAxes(direction);
    send_agent_update(true, false);
    snapAvatarBody(direction);
    snapRemoteAvatarBody(mTargetAvatar);
    mTargetAvatar = nullptr;
}

void FSAvatarAlignBase::rotateAgentTo(F32 target_deg)
{
    F32 yaw_rad = target_deg * DEG_TO_RAD;
    LLVector3 look_at(sinf(yaw_rad), cosf(yaw_rad), 0.f);
    applyRotation(look_at);
}

void FSAvatarAlignBase::onClickCardinal(F32 target_deg)
{
    rotateAgentTo(target_deg);
}

void FSAvatarAlignBase::onClickRotate(F32 delta_deg)
{
    LLVector3 at = gAgent.getFrameAgent().getAtAxis();
    at.mV[VZ] = 0.f;
    at.normalize();
    F32 yaw_deg = atan2f(at.mV[VX], at.mV[VY]) * RAD_TO_DEG;
    rotateAgentTo(fmodf(yaw_deg + delta_deg + 360.f, 360.f));
}

void FSAvatarAlignBase::onClickNearest()
{
    LLVector3 at = gAgent.getFrameAgent().getAtAxis();
    at.mV[VZ] = 0.f;
    at.normalize();
    F32 yaw_deg = atan2f(at.mV[VX], at.mV[VY]) * RAD_TO_DEG;
    yaw_deg = fmodf(yaw_deg + 360.f, 360.f);
    F32 nearest_deg = fmodf((F32)(ll_round(yaw_deg / 45.f) * 45), 360.f);
    rotateAgentTo(nearest_deg);
}

bool FSAvatarAlignBase::isAvatarInRange(LLVOAvatar* avatar) const
{
    if (!avatar || avatar->isDead())
        return false;
    return dist_vec(avatar->getPositionAgent(), gAgent.getPositionAgent()) <= MAX_FACE_DISTANCE;
}

void FSAvatarAlignBase::faceAvatar(LLVOAvatar* avatar)
{
    if (!avatar || !isAgentAvatarValid())
        return;

    mTargetAvatar = avatar;

    LLVector3 direction = avatar->getPositionAgent() - gAgentAvatarp->getPositionAgent();
    direction.mV[VZ] = 0.f;
    direction.normalize();

    applyRotation(direction);
}

void FSAvatarAlignBase::onClickFaceNearestAvatar()
{
    if (!isAgentAvatarValid())
        return;

    LLVector3   my_pos         = gAgent.getPositionAgent();
    LLVOAvatar* nearest        = nullptr;
    F32         nearest_dist_sq = F32_MAX;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (avatar->isDead() || avatar->isControlAvatar() || avatar->isSelf())
            continue;
        if (isAvatarFlying(avatar))
            continue;

        F32 dist_sq = dist_vec_squared(avatar->getPositionAgent(), my_pos);
        if (dist_sq > MAX_FACE_DISTANCE * MAX_FACE_DISTANCE)
            continue;
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

    faceAvatar(nearest);
}

// ============================================================
// FSFloaterAvatarAlign  (full mode)
// ============================================================

FSFloaterAvatarAlign::FSFloaterAvatarAlign(const LLSD& key)
    : FSAvatarAlignBase(key)
{
}

bool FSFloaterAvatarAlign::postBuild()
{
    childSetAction("btn_rotate_left_90",  [this](void*) { onClickRotate(-90.f);         }, this);
    childSetAction("btn_rotate_left_45",  [this](void*) { onClickRotate(-45.f);         }, this);
    childSetAction("btn_rotate_right_45", [this](void*) { onClickRotate( 45.f);         }, this);
    childSetAction("btn_rotate_right_90", [this](void*) { onClickRotate( 90.f);         }, this);
    childSetAction("btn_rotate_left_10",  [this](void*) { onClickRotate(-10.f);         }, this);
    childSetAction("btn_rotate_left_1",   [this](void*) { onClickRotate( -1.f);         }, this);
    childSetAction("btn_rotate_right_1",  [this](void*) { onClickRotate(  1.f);         }, this);
    childSetAction("btn_rotate_right_10", [this](void*) { onClickRotate( 10.f);         }, this);
    childSetAction("btn_nearest",         [this](void*) { onClickNearest();             }, this);
    childSetAction("btn_avatar",          [this](void*) { onClickFaceNearestAvatar();   }, this);

    return true;
}

void FSFloaterAvatarAlign::onOpen(const LLSD& key)
{
}

void FSFloaterAvatarAlign::onToggleMode()
{
    gSavedSettings.setBOOL("AvatarAlignMini", true);
    LLRect rect = getRect();
    closeFloater(false);
    FSFloaterAvatarAlignMini* mini = LLFloaterReg::showTypedInstance<FSFloaterAvatarAlignMini>("avatar_align_mini");
    if (mini)
    {
        // Keep the top-left corner fixed
        mini->setOrigin(rect.mLeft, rect.mBottom + rect.getHeight() - mini->getRect().getHeight());
    }
}

// ============================================================
// FSFloaterAvatarAlignMini  (mini mode)
// ============================================================

FSFloaterAvatarAlignMini::FSFloaterAvatarAlignMini(const LLSD& key)
    : FSAvatarAlignBase(key)
{
}

bool FSFloaterAvatarAlignMini::postBuild()
{
    childSetAction("btn_rotate_left_1",  [this](void*) { onClickRotate(-1.f);           }, this);
    childSetAction("btn_rotate_right_1", [this](void*) { onClickRotate( 1.f);           }, this);
    childSetAction("btn_avatar",         [this](void*) { onClickFaceNearestAvatar();    }, this);

    return true;
}

void FSFloaterAvatarAlignMini::onOpen(const LLSD& key)
{
}

void FSFloaterAvatarAlignMini::onToggleMode()
{
    gSavedSettings.setBOOL("AvatarAlignMini", false);
    LLRect rect = getRect();
    closeFloater(false);
    FSFloaterAvatarAlign* full = LLFloaterReg::showTypedInstance<FSFloaterAvatarAlign>("avatar_align");
    if (full)
    {
        full->setOrigin(rect.mLeft, rect.mBottom + rect.getHeight() - full->getRect().getHeight());
    }
}
