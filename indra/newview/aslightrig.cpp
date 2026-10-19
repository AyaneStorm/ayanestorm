/**
 * @file aslightrig.cpp
 * @author chanayane@firestorm
 * @brief See aslightrig.h
 */

#include "llviewerprecompiledheaders.h"

#include "aslightrig.h"
#include "aslightrigrenderer.h"

#include "indra_constants.h"
#include "llagent.h"
#include "llvoavatarself.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "lljoint.h"
#include "llquaternion.h"

extern LLPipeline gPipeline;
extern LLPointer<LLVOAvatarSelf> gAgentAvatarp;

namespace
{
    // Tiny scale + fullbright alpha-0 face keeps the object a light source
    // without a visible mesh -- no "invisiprim" primitive exists in this
    // codebase, so a near-zero sphere is the pragmatic stand-in.
    const F32 LIGHT_OBJECT_SCALE = 0.01f;

}

ASLightRig::ASLightRig()
:   mId(LLUUID::generateNewID()),
    mName("Light"),
    mAnchorJoint("mChest"),
    mDistance(0.5f),
    mHeight(0.f),
    mAzimuth(0.f),
    mIntensity(1.f),
    mRadius(5.f),
    mFalloff(1.f),
    mColor(1.f, 1.f, 1.f),
    mSpot(false),
    mAimJoint("mNeck"),
    mAimYaw(0.f),
    mAimPitch(0.f),
    mSpotFOV(F_PI * 0.5f),      // LLLightImageParams default
    mSpotFocus(0.f),
    mSpotAmbiance(0.f),
    mEnabled(true),
    mPositionAgent(LLVector3::zero),
    mAimDirection(-LLVector3::z_axis)
{
}

ASLightRig::~ASLightRig()
{
    destroy();
}

void ASLightRig::create()
{
    if (ASLightRigRenderer::usesShaderBackend() ||
        (isCreated() && !mObject->isDead()) || !gAgentAvatarp)
    {
        return;
    }

    // A viewer-local object can be killed with the region that owned it.
    // Release that stale reference so teleport recovery can create a new one.
    if (mObject.notNull())
    {
        mObject = nullptr;
    }

    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }

    LLVOVolume* vobjp = static_cast<LLVOVolume*>(
        gObjectList.createObjectViewer(LL_PCODE_VOLUME, region));
    if (!vobjp)
    {
        return;
    }

    LLVolumeParams volume_params;
    volume_params.setType(LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE);
    volume_params.setBeginAndEndS(0.f, 1.f);
    volume_params.setBeginAndEndT(0.f, 1.f);
    volume_params.setRatio(1.f, 1.f);
    volume_params.setShear(0.f, 0.f);
    vobjp->setVolume(volume_params, 0);
    vobjp->setScale(LLVector3(LIGHT_OBJECT_SCALE, LIGHT_OBJECT_SCALE, LIGHT_OBJECT_SCALE), false);
    vobjp->setPositionAgent(gAgentAvatarp->getPositionAgent());

    // IMG_INVISIBLE is an alpha-only (GL_ALPHA) texture, which routes this
    // face to the true non-rendering PASS_INVISIBLE draw pass -- a fullbright
    // alpha-0 face is not sufficient (LLFace::mFaceColor, which actually
    // gates the invisible-alpha pass, is never touched by ordinary prim
    // texture-entry alpha) and remains visible, including in snapshots.
    vobjp->setTETexture(0, IMG_INVISIBLE);

    // This object was never rezzed through the server (local id stays 0), so
    // a right-click "Delete" would send a DeRezObject the region silently
    // ignores -- the prim would keep existing as an orphaned "ghost" light
    // while the user believes it's gone. Prevent mouse selection entirely,
    // matching how other purely-viewer-local objects (LLVOWater, LLVOSky,
    // LLVOPartGroup) opt out of picking.
    vobjp->mbCanSelect = false;

    gPipeline.createObject(vobjp);

    mObject = vobjp;

    mObject->setIsLight(mEnabled);
    applyParams();
    updateTransform();
}

void ASLightRig::destroy()
{
    if (mObject.notNull())
    {
        mObject->markDead();
        mObject = nullptr;
    }
}

void ASLightRig::updateTransform()
{
    if (ASLightRigRenderer::usesShaderBackend() && mObject.notNull())
    {
        destroy();
    }

    if (!mEnabled || !gAgentAvatarp)
    {
        return;
    }

    LLViewerRegion* region = gAgent.getRegion();
    if (!region || gAgentAvatarp->getRegion() != region)
    {
        return;
    }

    LLJoint* joint = gAgentAvatarp->getJoint(mAnchorJoint);
    if (!joint)
    {
        joint = gAgentAvatarp->getJoint("mChest");
    }
    if (!joint)
    {
        return;
    }

    const LLVector3 joint_pos = joint->getWorldPosition();
    const LLQuaternion joint_rot = joint->getWorldRotation();

    // Offset expressed in the joint's local frame: forward = distance,
    // up = height, azimuth rotates around the joint's up axis (0 = front).
    LLVector3 offset(mDistance, 0.f, mHeight);
    offset.rotVec(LLQuaternion(mAzimuth * DEG_TO_RAD, LLVector3::z_axis));
    offset.rotVec(joint_rot);

    mPositionAgent = joint_pos + offset;

    LLQuaternion spot_rot;
    if (mSpot)
    {
        spot_rot = computeSpotRotation(joint_pos);
        mAimDirection = LLVector3(0.f, 0.f, -1.f) * spot_rot;
    }

    if (ASLightRigRenderer::usesShaderBackend())
    {
        return;
    }

    // Viewer-local objects retain their creation region. Teleports can kill
    // the old object, and merely moving a surviving old-region object uses
    // the wrong region-to-agent transform. Recreate it in the settled agent
    // region so the light resumes following the avatar.
    if (mObject.isNull() || mObject->isDead() || mObject->getRegion() != region)
    {
        destroy();
        create();
        return;
    }

    mObject->setPositionAgent(mPositionAgent);
    if (mSpot)
    {
        // Spot lights project along the object's -Z (LLPipeline::setupSpotLight).
        mObject->setRotation(spot_rot);
    }
}

LLQuaternion ASLightRig::computeSpotRotation(const LLVector3& anchor_pos)
{
    // Aim at the chosen joint; fall back to the anchor joint if it is
    // missing or sits on the light itself.
    LLVector3 forward;
    LLJoint* aim_joint = gAgentAvatarp->getJoint(mAimJoint);
    if (aim_joint)
    {
        forward = aim_joint->getWorldPosition() - mPositionAgent;
    }
    if (!aim_joint || forward.magVecSquared() < 1.e-6f)
    {
        forward = anchor_pos - mPositionAgent; // non-zero: mDistance >= 0.05
    }
    forward.normalize();

    // Keep the projector image upright: its up axis follows world up,
    // except when the light points almost straight up or down.
    LLVector3 up = LLVector3::z_axis;
    if (llabs(forward * up) > 0.99f)
    {
        up = LLVector3::x_axis;
    }
    const LLVector3 z_axis = -forward;
    LLVector3 x_axis = up % z_axis;
    x_axis.normalize();
    const LLVector3 y_axis = z_axis % x_axis;
    const LLQuaternion base(x_axis, y_axis, z_axis);

    // Offsets in the light's own frame: pitch about its X (right) axis,
    // then yaw about its Y (up) axis, so yaw stays level with the horizon.
    const LLQuaternion offset =
        LLQuaternion(mAimPitch * DEG_TO_RAD, LLVector3::x_axis) *
        LLQuaternion(mAimYaw * DEG_TO_RAD, LLVector3::y_axis);
    return offset * base;
}

LLVector3 ASLightRig::getObjectPositionAgent() const
{
    return mPositionAgent;
}

LLUUID ASLightRig::getObjectId() const
{
    return mObject.notNull() ? mObject->getID() : LLUUID::null;
}

void ASLightRig::applyParams()
{
    if (mObject.isNull())
    {
        return;
    }

    mObject->setLightSRGBColor(mColor);
    mObject->setLightIntensity(mIntensity);
    mObject->setLightRadius(mRadius);
    mObject->setLightFalloff(mFalloff);

    // A projector image is what makes an LLVOVolume light a spot
    // (LLLightImageParams::isLightSpotlight()); clearing it restores a point.
    if (mSpot)
    {
        mObject->setLightTextureID(mSpotTexture.notNull() ? mSpotTexture : IMG_WHITE);
        mObject->setSpotLightParams(LLVector3(mSpotFOV, mSpotFocus, mSpotAmbiance));
    }
    else
    {
        mObject->setLightTextureID(LLUUID::null);
    }
}

void ASLightRig::setEnabled(bool enabled)
{
    mEnabled = enabled;
    if (mObject.notNull())
    {
        mObject->setIsLight(mEnabled);
    }
}

LLSD ASLightRig::toLLSD() const
{
    LLSD data;
    data["name"] = mName;
    data["joint"] = mAnchorJoint;
    data["distance"] = mDistance;
    data["height"] = mHeight;
    data["azimuth"] = mAzimuth;
    data["intensity"] = mIntensity;
    data["radius"] = mRadius;
    data["falloff"] = mFalloff;
    data["color"] = mColor.getValue();
    data["spot"] = mSpot;
    data["aim_joint"] = mAimJoint;
    data["aim_yaw"] = mAimYaw;
    data["aim_pitch"] = mAimPitch;
    data["spot_fov"] = mSpotFOV;
    data["spot_focus"] = mSpotFocus;
    data["spot_ambiance"] = mSpotAmbiance;
    data["spot_texture"] = mSpotTexture;
    return data;
}

void ASLightRig::fromLLSD(const LLSD& data)
{
    mName = data["name"].asString();
    mAnchorJoint = data["joint"].asString();
    mDistance = (F32)data["distance"].asReal();
    mHeight = (F32)data["height"].asReal();
    mAzimuth = (F32)data["azimuth"].asReal();
    mIntensity = (F32)data["intensity"].asReal();
    mRadius = (F32)data["radius"].asReal();
    mFalloff = (F32)data["falloff"].asReal();
    mColor.setValue(data["color"]);

    // Spot keys are absent from older presets: keep the defaults then.
    if (data.has("spot"))          mSpot = data["spot"].asBoolean();
    if (data.has("aim_joint"))     mAimJoint = data["aim_joint"].asString();
    if (data.has("aim_yaw"))       mAimYaw = (F32)data["aim_yaw"].asReal();
    if (data.has("aim_pitch"))     mAimPitch = (F32)data["aim_pitch"].asReal();
    if (data.has("spot_fov"))      mSpotFOV = (F32)data["spot_fov"].asReal();
    if (data.has("spot_focus"))    mSpotFocus = (F32)data["spot_focus"].asReal();
    if (data.has("spot_ambiance")) mSpotAmbiance = (F32)data["spot_ambiance"].asReal();
    if (data.has("spot_texture"))  mSpotTexture = data["spot_texture"].asUUID();
}
