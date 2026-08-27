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
#include "lluuid.h"
#include "v3math.h"
#include "llsd.h"

// One local light: its placement relative to an avatar joint, its light
// parameters, and the live viewer object that actually emits the light.
class ASLightRig
{
public:
    ASLightRig();
    ~ASLightRig();

    // Creates the underlying local-only LLVOVolume and applies current params.
    // No-op if already created.
    void create();

    // Marks the underlying object dead and releases it. Safe to call multiple times.
    void destroy();

    bool isCreated() const { return mObject.notNull(); }

    // Object id of the live viewer object, so isolate-background mode can
    // recognize and skip our own light-rig objects while hiding everything
    // else. Returns a null id if not created.
    LLUUID getObjectId() const;

    // Current world position of the live object, for beacon rendering.
    // Returns a zero vector if not created.
    LLVector3 getObjectPositionAgent() const;

    // Recomputes this light's world position from its anchor joint's current
    // world transform plus mDistance/mHeight/mAzimuth, and pushes it to the
    // live object. Called every idle tick by the owning floater for every
    // rig in its list, regardless of selection, so all lights track the
    // avatar continuously. No-op if disabled or not created.
    void updateTransform();

    // Pushes mIntensity/mRadius/mFalloff/mColor to the live LLVOVolume light
    // setters. Safe to call before create() (values are just cached).
    void applyParams();

    // Turns this light's contribution to the scene on/off without destroying
    // its object -- cheap, keeps params/position intact.
    void setEnabled(bool enabled);
    bool isEnabled() const { return mEnabled; }

    LLSD toLLSD() const;
    void fromLLSD(const LLSD& data);

    LLUUID mId;
    std::string mName;
    std::string mAnchorJoint;   // e.g. "mChest", "mHead", "mPelvis"
    F32 mDistance;              // meters, along the joint's forward axis
    F32 mHeight;                // meters, vertical offset
    F32 mAzimuth;                // degrees, around the joint (0 = front)
    F32 mIntensity;
    F32 mRadius;
    F32 mFalloff;
    LLColor3 mColor;

private:
    bool mEnabled;
    LLPointer<LLVOVolume> mObject;
};

#endif // AS_LIGHTRIG_H
