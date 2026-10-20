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
#include "llviewernetwork.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llworld.h"
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
    mPlacement(PLACEMENT_FOLLOW),
    mEnabled(true),
    mLightOn(true),
    mDormant(false),
    mPositioned(false),
    mPositionAgent(LLVector3::zero),
    mAimDirection(-LLVector3::z_axis)
{
}

ASLightRig::~ASLightRig()
{
    destroy();
}

// static
std::string ASLightRig::currentGrid()
{
    return LLGridManager::getInstance()->getGridId();
}

LLViewerRegion* ASLightRig::getHomeRegion() const
{
    if (mGrid != currentGrid())
    {
        return nullptr;
    }
    // Global coordinates can map to a different region if regions move on
    // the map; only the region with the expected name is home.
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(mPositionGlobal);
    return (region && region->getName() == mRegionName) ? region : nullptr;
}

LLVector3d ASLightRig::getFixedPositionGlobal() const
{
    return mPositionGlobal + LLVector3d(mFixedOffset);
}

LLViewerRegion* ASLightRig::getTargetRegion() const
{
    return isFixed() ? getHomeRegion() : gAgent.getRegion();
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

    // Following lights live in the agent's region; fixed lights in their
    // home region, and not at all while it is unloaded (dormant).
    LLViewerRegion* region = getTargetRegion();
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
    vobjp->setPositionAgent(isFixed() ? gAgent.getPosAgentFromGlobal(getFixedPositionGlobal())
                                      : gAgentAvatarp->getPositionAgent());

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

    mObject->setIsLight(isEnabled());
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

    // Fixed lights sleep while their home region is unloaded (teleported
    // away, beyond draw distance) and wake when it loads again. Checked even
    // when switched off, so the list can show which lights are away.
    LLViewerRegion* region = nullptr;
    if (isFixed())
    {
        region = getHomeRegion();
        mDormant = (region == nullptr);
        if (mDormant)
        {
            destroy();
            return;
        }
    }
    else
    {
        mDormant = false;
    }

    if (!isEnabled() || !gAgentAvatarp)
    {
        return;
    }

    // Spot fallback target when the aim joint is missing.
    LLVector3 fallback_target;
    if (isFixed())
    {
        // Converted every frame: agent coordinates shift on region crossings.
        mPositionAgent = gAgent.getPosAgentFromGlobal(getFixedPositionGlobal());
        fallback_target = gAgentAvatarp->getPositionAgent();
    }
    else
    {
        region = gAgent.getRegion();
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
        fallback_target = joint_pos;
    }
    mPositioned = true;

    LLQuaternion spot_rot;
    if (mSpot)
    {
        spot_rot = computeSpotRotation(fallback_target);
        mAimDirection = LLVector3(0.f, 0.f, -1.f) * spot_rot;
    }

    if (ASLightRigRenderer::usesShaderBackend())
    {
        return;
    }

    // Viewer-local objects retain their creation region. Teleports can kill
    // the old object, and merely moving a surviving old-region object uses
    // the wrong region-to-agent transform. Recreate it in the target region
    // (the settled agent region, or a fixed light's home region).
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

LLQuaternion ASLightRig::computeSpotRotation(const LLVector3& fallback_target)
{
    // Aim at the chosen joint; fall back to fallback_target if it is
    // missing or sits on the light itself.
    LLVector3 forward;
    LLJoint* aim_joint = gAgentAvatarp->getJoint(mAimJoint);
    if (aim_joint)
    {
        forward = aim_joint->getWorldPosition() - mPositionAgent;
    }
    if (!aim_joint || forward.magVecSquared() < 1.e-6f)
    {
        forward = fallback_target - mPositionAgent;
    }
    if (forward.magVecSquared() < 1.e-6f)
    {
        forward.setVec(0.f, 0.f, -1.f); // light on the target: point down
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

bool ASLightRig::placeAt(const LLVector3d& pos_global)
{
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global);
    if (!region)
    {
        return false;
    }
    mPlacement = PLACEMENT_FIXED;
    mPositionGlobal = pos_global;
    mFixedOffset.clearVec();    // offsets restart centered on the new base
    mRegionName = region->getName();
    mGrid = currentGrid();
    mDormant = false;
    updateTransform(); // moves, or recreates the object in the new home region
    return true;
}

bool ASLightRig::makeFixedHere(const LLVector3d& fallback_global)
{
    if (isFixed())
    {
        return true;
    }
    return placeAt(mPositioned ? gAgent.getPosGlobalFromAgent(mPositionAgent) : fallback_global);
}

void ASLightRig::makeFollow()
{
    mPlacement = PLACEMENT_FOLLOW;
    mDormant = false;
    updateTransform(); // recreates the object in the agent's region if needed
}

void ASLightRig::setFixedOffset(const LLVector3& offset)
{
    mFixedOffset = offset;
    mFixedOffset.clamp(-MAX_FIXED_OFFSET, MAX_FIXED_OFFSET);
    updateTransform();
}

bool ASLightRig::belongsToRegion(const LLViewerRegion* region) const
{
    return isFixed() && region && region->getName() == mRegionName && mGrid == currentGrid();
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
        mObject->setIsLight(isEnabled());
    }
}

void ASLightRig::setLightOn(bool on)
{
    mLightOn = on;
    if (mObject.notNull())
    {
        mObject->setIsLight(isEnabled());
    }
}

LLSD ASLightRig::toLLSD() const
{
    LLSD data;
    data["name"] = mName;
    data["on"] = mLightOn;
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
    data["placement"] = isFixed() ? "fixed" : "follow";
    if (isFixed())
    {
        data["pos_global"] = mPositionGlobal.getValue();
        data["fixed_offset"] = mFixedOffset.getValue();
        data["region"] = mRegionName;
        data["grid"] = mGrid;
    }
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

    // Keys below are absent from older presets: keep the defaults then
    // (light on, point light aimed at mNeck, following the avatar).
    if (data.has("on"))            mLightOn = data["on"].asBoolean();
    if (data.has("spot"))          mSpot = data["spot"].asBoolean();
    if (data.has("aim_joint"))     mAimJoint = data["aim_joint"].asString();
    if (data.has("aim_yaw"))       mAimYaw = (F32)data["aim_yaw"].asReal();
    if (data.has("aim_pitch"))     mAimPitch = (F32)data["aim_pitch"].asReal();
    if (data.has("spot_fov"))      mSpotFOV = (F32)data["spot_fov"].asReal();
    if (data.has("spot_focus"))    mSpotFocus = (F32)data["spot_focus"].asReal();
    if (data.has("spot_ambiance")) mSpotAmbiance = (F32)data["spot_ambiance"].asReal();
    if (data.has("spot_texture"))  mSpotTexture = data["spot_texture"].asUUID();

    if (data["placement"].asString() == "fixed" && data.has("pos_global"))
    {
        mPlacement = PLACEMENT_FIXED;
        mPositionGlobal.setValue(data["pos_global"]);
        if (data.has("fixed_offset"))
        {
            mFixedOffset.setValue(data["fixed_offset"]);
            mFixedOffset.clamp(-MAX_FIXED_OFFSET, MAX_FIXED_OFFSET);
        }
        mRegionName = data["region"].asString();
        mGrid = data["grid"].asString();
    }
}
