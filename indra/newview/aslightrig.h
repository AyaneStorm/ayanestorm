/**
 * @file aslightrig.h
 * @author chanayane@firestorm
 * @brief A single viewer-local, self-only light source used by the AyaneStorm
 *        self-lighting floater. Not sent to the server, not in inventory, not
 *        visible to other viewers.
 */

#ifndef AS_LIGHTRIG_H
#define AS_LIGHTRIG_H

#include "llvovolume.h"
#include "v3color.h"
#include "v3dmath.h"
#include "lluuid.h"
#include "v3math.h"
#include "llquaternion.h"
#include "llsd.h"

class LLViewerRegion;

// One local light: its placement (following an avatar joint, or fixed at a
// world position), its light parameters, and the live viewer object that
// actually emits the light.
class ASLightRig
{
public:
    // Follow: placed relative to an avatar joint every frame ("worn").
    // Fixed: stays at mPositionGlobal in its home region (mRegionName on
    // mGrid); dormant -- no object, no light -- while that region is not
    // loaded, and recreated when it is again.
    enum Placement
    {
        PLACEMENT_FOLLOW = 0,
        PLACEMENT_FIXED = 1
    };

    ASLightRig();
    ~ASLightRig();

    // Creates the underlying local-only LLVOVolume and applies current params.
    // No-op if already created, in the shader backend, or while dormant.
    void create();

    // Marks the underlying object dead and releases it. Safe to call multiple times.
    void destroy();

    bool isCreated() const { return mObject.notNull(); }

    // Object id of the live viewer object, so isolate-background mode can
    // recognize and skip our own light-rig objects while hiding everything
    // else. Returns a null id if not created.
    LLUUID getObjectId() const;

    // Current world position of the live object, for beacon rendering.
    // Also available in the shader backend, which has no live object.
    LLVector3 getObjectPositionAgent() const;

    // Unit direction a spot light projects along (agent frame), for beacon
    // rendering. Only meaningful while mSpot is set; computed every
    // updateTransform() from the aim joint plus the yaw/pitch offsets.
    LLVector3 getAimDirection() const { return mAimDirection; }

    // Follow: recomputes the position from the anchor joint's current world
    // transform plus mDistance/mHeight/mAzimuth. Fixed: converts
    // mPositionGlobal to agent space, or goes dormant if the home region is
    // not loaded. Pushes the result to the live object, recreating it in the
    // right region when needed. Called every idle tick by the owning floater
    // for every rig in its list.
    void updateTransform();

    // Pushes mIntensity/mRadius/mFalloff/mColor to the live LLVOVolume light
    // setters. Safe to call before create() (values are just cached).
    void applyParams();

    // Master switch state, set by the floater for every light.
    void setEnabled(bool enabled);
    // Per-light on/off, saved with the light. Independent of the master.
    void setLightOn(bool on);
    bool isLightOn() const { return mLightOn; }
    // True when the light should emit: master on and this light on.
    bool isEnabled() const { return mEnabled && mLightOn; }

    // Placement
    bool isFixed() const { return mPlacement == PLACEMENT_FIXED; }
    // Fixed light whose home region is not currently loaded.
    bool isDormant() const { return mDormant; }
    // Fixes the light at a global position (its base location), binding it
    // to the loaded region that contains it (this also revives a dormant
    // light in a new region) and resetting the offset to zero. Returns false
    // and changes nothing if no loaded region contains pos.
    bool placeAt(const LLVector3d& pos_global);
    // Switches to Fixed at the light's current position, or at fallback_global
    // if the light has never been positioned (e.g. created while disabled).
    bool makeFixedHere(const LLVector3d& fallback_global);
    void makeFollow();
    // Offset of a fixed light from its base location, in world axes,
    // clamped to +/- MAX_FIXED_OFFSET meters per axis. The home region stays
    // the base location's region.
    void setFixedOffset(const LLVector3& offset);
    static constexpr F32 MAX_FIXED_OFFSET = 10.f;
    // Fixed light whose home region is region (same name, same grid).
    bool belongsToRegion(const LLViewerRegion* region) const;

    // Identifier of the grid currently logged in to.
    static std::string currentGrid();

    LLSD toLLSD() const;
    void fromLLSD(const LLSD& data);

    LLUUID mId;
    std::string mName;
    std::string mAnchorJoint;   // e.g. "mChest", "mHead", "mPelvis"
    F32 mDistance;              // meters, along the joint's forward axis
    F32 mHeight;                // meters, along the joint's up axis
    F32 mAzimuth;                // degrees, around the joint (0 = front)
    F32 mIntensity;
    F32 mRadius;
    F32 mFalloff;
    LLColor3 mColor;

    // Spot light (LLVOVolume backend only; the shader backend stays point).
    // A spot is a light with a projector image: it projects along the
    // object's -Z axis, which updateTransform() aims at mAimJoint.
    bool mSpot;
    std::string mAimJoint;      // joint the spot aims at, e.g. "mNeck"
    F32 mAimYaw;                // degrees, positive turns the aim left
    F32 mAimPitch;              // degrees, positive turns the aim up
    F32 mSpotFOV;               // radians, as LLLightImageParams
    F32 mSpotFocus;
    F32 mSpotAmbiance;
    LLUUID mSpotTexture;        // projector image; null = plain white

    // Fixed placement (meaningful when mPlacement == PLACEMENT_FIXED)
    Placement mPlacement;
    LLVector3d mPositionGlobal; // base location, set by placement
    LLVector3 mFixedOffset;     // meters from the base location, world axes
    std::string mRegionName;    // home region (the base's), shown while dormant
    std::string mGrid;          // grid the position belongs to

private:
    // Rotation whose -Z axis points from mPositionAgent at the aim joint
    // (or at fallback_target if that joint is missing), with the yaw/pitch
    // offsets applied in the light's own frame.
    LLQuaternion computeSpotRotation(const LLVector3& fallback_target);

    // Loaded region the light lives in: the agent's region when following,
    // the home region when fixed (null while dormant).
    LLViewerRegion* getTargetRegion() const;
    LLViewerRegion* getHomeRegion() const;
    // Base location plus offset.
    LLVector3d getFixedPositionGlobal() const;

    bool mEnabled;              // master switch
    bool mLightOn;              // per-light switch
    bool mDormant;
    bool mPositioned;           // mPositionAgent has been computed at least once
    // Cached independently of mObject so shader-only lights and beacons use
    // the same position without creating viewer geometry.
    LLVector3 mPositionAgent;
    LLVector3 mAimDirection;
    LLPointer<LLVOVolume> mObject;
};

#endif // AS_LIGHTRIG_H
