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

void FSFloaterAvatarAlign::drawCompass()
{
    LLRect local = getLocalRect();
    S32 header_h = getHeaderHeight();

    // Reserve ~95px at the bottom for buttons + checkbox.
    S32 avail_h = local.getHeight() - header_h - 95;
    S32 R = llclamp(llmin(local.getWidth() / 2 - 27, avail_h / 2), 40, 90);

    mCompassCX = local.getCenterX();
    mCompassCY = local.mTop - header_h - 30 - R;
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

    // 4 compass arm kite shapes
    // Each arm: tip at (R-4), left/right base points at R*0.35 rotated ±90°.
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

    // Center dot
    gGL.color4f(0.20f, 0.20f, 0.20f, 1.f);
    gl_circle_2d(cx, cy, 5.f, 16, TRUE);
    gGL.color4f(0.50f, 0.50f, 0.50f, 1.f);
    gl_circle_2d(cx, cy, 5.f, 16, FALSE);

    // Heading needle — gold triangle pointing in avatar's current facing direction.
    LLVector3 at = gAgent.getAtAxis();
    at.mV[VZ] = 0.f;
    if (at.normalize() > 0.01f)
    {
        // SL: X=East, Y=North — maps directly to GL screen X/Y (y-up).
        F32 nl = tipR * 0.88f;
        F32 pw = 4.f; // half-width of needle base
        F32 nx  = cx + nl * at.mV[VX];
        F32 ny  = cy + nl * at.mV[VY];
        F32 bx1 = cx - at.mV[VY] * pw;  F32 by1 = cy + at.mV[VX] * pw;
        F32 bx2 = cx + at.mV[VY] * pw;  F32 by2 = cy - at.mV[VX] * pw;

        gGL.begin(LLRender::TRIANGLES);
        gGL.color4f(1.f, 0.85f, 0.f, 0.95f);
        gGL.vertex2f(nx, ny); gGL.vertex2f(bx1, by1); gGL.vertex2f(bx2, by2);
        gGL.end();
    }

    // Cardinal labels
    LLFontGL* font   = LLFontGL::getFontSansSerifSmall();
    S32       ld     = R + 12;
    LLColor4  col_n(1.f, 0.55f, 0.55f, 1.f);
    LLColor4  col_o(0.90f, 0.90f, 0.90f, 1.f);

    font->renderUTF8("N", 0, cx,          (F32)(mCompassCY + ld), col_n, LLFontGL::HCENTER, LLFontGL::BOTTOM,   LLFontGL::BOLD,   LLFontGL::DROP_SHADOW);
    font->renderUTF8("S", 0, cx,          (F32)(mCompassCY - ld), col_o, LLFontGL::HCENTER, LLFontGL::TOP,      LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    font->renderUTF8("E", 0, (F32)(mCompassCX + ld), (F32)mCompassCY, col_o, LLFontGL::LEFT,    LLFontGL::VCENTER,  LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    font->renderUTF8("W", 0, (F32)(mCompassCX - ld), (F32)mCompassCY, col_o, LLFontGL::RIGHT,   LLFontGL::VCENTER,  LLFontGL::NORMAL, LLFontGL::NO_SHADOW);

    // Current bearing in degrees, centred below the compass.
    LLVector3 hat = gAgent.getAtAxis();
    hat.mV[VZ] = 0.f;
    hat.normalize();
    F32 bearing = fmodf(atan2f(hat.mV[VX], hat.mV[VY]) * RAD_TO_DEG + 360.f, 360.f);
    std::string bearing_str = llformat("%03.0f\xC2\xB0", bearing); // e.g. "045°"
    font->renderUTF8(bearing_str, 0, cx, (F32)(mCompassCY - R - 30),
        LLColor4(0.85f, 0.85f, 0.85f, 1.f), LLFontGL::HCENTER, LLFontGL::TOP,
        LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
}

bool FSFloaterAvatarAlign::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mCompassR > 0)
    {
        S32 dx   = x - mCompassCX;
        S32 dy   = y - mCompassCY;
        F32 dist = sqrtf((F32)(dx * dx + dy * dy));

        if (dist <= (F32)mCompassR)
        {
            if (dist < (F32)mCompassR * 0.25f)
            {
                // Centre tap: face nearest avatar.
                onClickFaceNearestAvatar();
            }
            else
            {
                // Outer tap: determine cardinal from angle.
                // atan2f(dx, dy) gives angle from +Y (North), positive clockwise.
                F32 deg = atan2f((F32)dx, (F32)dy) * RAD_TO_DEG;
                if      (deg >= -45.f  && deg <  45.f)  onClickCardinal(  0.f); // N
                else if (deg >=  45.f  && deg < 135.f)  onClickCardinal( 90.f); // E
                else if (deg >= 135.f  || deg < -135.f) onClickCardinal(180.f); // S
                else                                     onClickCardinal(270.f); // W
            }
            return true;
        }
    }
    return LLFloater::handleMouseDown(x, y, mask);
}

void FSFloaterAvatarAlign::draw()
{
    LLFloater::draw();
    drawCompass();

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
