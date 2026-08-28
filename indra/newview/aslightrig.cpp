/**
 * @file aslightrig.cpp
 * @author chanayane@firestorm
 * @brief See aslightrig.h
 */

#include "llviewerprecompiledheaders.h"

#include "aslightrig.h"

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
    mEnabled(true)
{
}

ASLightRig::~ASLightRig()
{
    destroy();
}

void ASLightRig::create()
{
    if (isCreated() || !gAgentAvatarp)
    {
        return;
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
    if (mObject.isNull() || !mEnabled || !gAgentAvatarp)
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

    mObject->setPositionAgent(joint_pos + offset);
}

LLVector3 ASLightRig::getObjectPositionAgent() const
{
    return mObject.notNull() ? mObject->getPositionAgent() : LLVector3::zero;
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
}
