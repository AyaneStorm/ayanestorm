/**
 * @file asdofautofocus.cpp
 * @author chanayane@firestorm
 * @brief Depth-of-field autofocus (ASDepthOfFieldFocusMode 1 and 2).
 */
#include "llviewerprecompiledheaders.h"
#include "asdofautofocus.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "asdofrenderer.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcharacter.h"
#include "lldrawable.h"
#include "llface.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llprogressview.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llmodel.h" // LLMeshSkinInfo
#include "llviewerobjectlist.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llvolume.h"
#include "pipeline.h"

extern bool gSnapshot; // display(): set for every snapshot capture

namespace
{
    // Sample grid: GRID_W x GRID_H area samples, plus one row whose first
    // texel is the scene distance at the tracked eyes (occlusion probe).
    constexpr S32 GRID_W = 64;
    constexpr S32 GRID_H = 32;
    constexpr S32 SAMPLE_COUNT = GRID_W * GRID_H;
    constexpr S32 TARGET_H = GRID_H + 1;
    constexpr S32 READBACK_FLOATS = GRID_W * TARGET_H;
    // Readbacks in flight; a frame with none free skips sampling.
    constexpr S32 SLOT_COUNT = 3;
    constexpr F32 MIN_DISTANCE = 0.1f;
    // Focus starts moving when the target differs by more than START_BAND
    // (relative) and snaps once within SETTLE_BAND: animation noise below
    // the band keeps the focus, and aperture-sampled accumulation, still.
    constexpr F32 START_BAND = 0.01f;
    constexpr F32 SETTLE_BAND = 0.003f;
    // Eyes count as visible unless the scene is nearer than this fraction
    // of their distance, minus a margin for noses, hair and eyelids.
    constexpr F32 EYE_VISIBLE_RATIO = 0.9f;
    constexpr F32 EYE_VISIBLE_MARGIN = 0.05f;
    // Human eyeball radius (metres): eye joint to the cornea. Used when the
    // mesh eyeball cannot be measured and no manual radius is set.
    constexpr F32 EYEBALL_RADIUS = 0.012f;
    // Eyeball measurement (measureEyes()): minimum weight of a vertex to an
    // eye joint, minimum vertex count of an eyeball; timings in seconds.
    constexpr F32 EYE_WEIGHT = 0.9f;
    constexpr size_t MIN_EYE_VERTICES = 8;
    constexpr F64 RETRY_SECONDS = 5.0;
    constexpr S32 MAX_TRIES = 3;
    // A longer gap between updates (autofocus off, renderer changed)
    // restarts from the caller's focus distance.
    constexpr U32 STALE_FRAMES = 30;

    enum FocusMode
    {
        FOCUS_POINT = 0, // Firestorm focus point
        FOCUS_AREA = 1,
        FOCUS_EYES = 2
    };

    enum LockMode
    {
        LOCK_DISTANCE = 0, // classic: the focus distance freezes
        LOCK_SUBJECT = 1   // the subject under the area is tracked
    };

    struct Slot
    {
        GLuint mBuffer = 0;
        GLsync mFence = 0;
        U32 mSequence = 0;
        bool mHasEyes = false;
        F32 mEyeDistance = 0.f;
        LLUUID mEyeAvatar;
    };

    // Subject followed while the focus lock is on (LOCK_SUBJECT).
    struct Subject
    {
        enum Type
        {
            NONE,
            EYES,   // an avatar's near eye (eyesPosition())
            JOINT,  // a point fixed to an avatar skeleton joint
            OBJECT  // a point fixed to an object
        };
        Type mType = NONE;
        LLUUID mID;          // avatar or object
        S32 mJoint = -1;     // index in the avatar skeleton (JOINT)
        LLVector3 mOffset;   // in the joint or object frame
        std::string mLabel;
    };

    LLGLSLShader sProgram;
    LLRenderTarget sTarget;
    Slot sSlots[SLOT_COUNT];
    U32 sSequence = 0;
    std::vector<F32> sReadback;
    std::vector<F32> sSorted;

    bool sHasTarget = false;   // a readback has produced a target
    F32 sTargetDistance = 0.f;
    bool sHasFocus = false;    // sCurrent is valid
    F32 sCurrent = 0.f;
    bool sMoving = false;
    U32 sLastFrame = 0;

    // Overlay state from the latest update.
    bool sEyesTracked = false; // latest readback focused on visible eyes
    LLUUID sEyesAvatar;        // whose eyes (sEyesTracked)
    bool sHasEyeCandidate = false;
    LLVector2 sEyeUV;

    Subject sSubject;
    bool sLockHandled = false;    // this focus lock press was processed
    bool sSubjectInView = false;  // the subject set the focus this frame
    LLVector2 sSubjectUV;

    const LLStaticHashedString U_AF_RECT("af_rect");
    const LLStaticHashedString U_EYE_UV("eye_uv");

    S32 focusMode()
    {
        static LLCachedControl<S32> mode(gSavedSettings, "ASDepthOfFieldFocusMode", 0);
        return llclamp((S32)mode, 0, 2);
    }

    // Autofocus area in scene UV (origin bottom left): min xy, max xy.
    // Square in pixels; ASDepthOfFieldAutofocusArea is its fraction of the
    // view height, X/Y its centre from the left and the top.
    LLVector4 areaRect()
    {
        static LLCachedControl<F32> cx(gSavedSettings, "ASDepthOfFieldAutofocusX", 0.5f);
        static LLCachedControl<F32> cy(gSavedSettings, "ASDepthOfFieldAutofocusY", 0.5f);
        static LLCachedControl<F32> area(gSavedSettings, "ASDepthOfFieldAutofocusArea", 0.3f);
        const F32 aspect = llmax(LLViewerCamera::getInstance()->getAspect(), 0.01f);
        const F32 half_v = llclamp((F32)area, 0.02f, 1.f) * 0.5f;
        const F32 half_u = half_v / aspect;
        const F32 u = llclamp((F32)cx, 0.f, 1.f);
        const F32 v = 1.f - llclamp((F32)cy, 0.f, 1.f);
        return LLVector4(llclamp(u - half_u, 0.f, 1.f), llclamp(v - half_v, 0.f, 1.f),
                         llclamp(u + half_u, 0.f, 1.f), llclamp(v + half_v, 0.f, 1.f));
    }

    // Scene UV of an agent-space point; false behind the near distance.
    bool projectToUV(const LLVector3& point, LLVector2& uv, F32& distance)
    {
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 d = point - camera->getOrigin();
        distance = d * camera->getAtAxis();
        if (distance < MIN_DISTANCE)
        {
            return false;
        }
        const F32 tan_half = tanf(camera->getView() * 0.5f);
        const F32 aspect = llmax(camera->getAspect(), 0.01f);
        uv.mV[VX] = 0.5f - 0.5f * (d * camera->getLeftAxis()) / (distance * tan_half * aspect);
        uv.mV[VY] = 0.5f + 0.5f * (d * camera->getUpAxis()) / (distance * tan_half);
        return true;
    }

    bool insideRect(const LLVector4& rect, const LLVector2& uv)
    {
        return uv.mV[VX] >= rect.mV[0] && uv.mV[VX] <= rect.mV[2] &&
               uv.mV[VY] >= rect.mV[1] && uv.mV[VY] <= rect.mV[3];
    }

    // An avatar's eye joints: mEyeLeft/Right, or the Bento
    // mFaceEyeAltLeft/Right (mesh heads may position their eyeballs on
    // either set through joint overrides).
    struct EyePair
    {
        LLJoint* mLeft = nullptr;
        LLJoint* mRight = nullptr;
    };

    EyePair eyePair(LLVOAvatar* avatar, bool alt)
    {
        EyePair pair;
        if (alt)
        {
            pair.mLeft = avatar->getJoint("mFaceEyeAltLeft");
            pair.mRight = avatar->getJoint("mFaceEyeAltRight");
        }
        if (!pair.mLeft || !pair.mRight)
        {
            pair.mLeft = avatar->mEyeLeftp;
            pair.mRight = avatar->mEyeRightp;
        }
        return pair;
    }

    // Eye nearer the camera (portraits focus on the near eye).
    LLJoint* nearerEye(const EyePair& pair)
    {
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const F32 left = (pair.mLeft->getWorldPosition() - camera->getOrigin()) * camera->getAtAxis();
        const F32 right = (pair.mRight->getWorldPosition() - camera->getOrigin()) * camera->getAtAxis();
        return left <= right ? pair.mLeft : pair.mRight;
    }

    // Eye joints searched in skin weights: mEyeLeft, mEyeRight,
    // mFaceEyeAltLeft, mFaceEyeAltRight (EYE_JOINTS order).
    constexpr S32 EYE_JOINTS = 4;

    // Eyeball vertices of one eye joint, per layer (face): mesh eyes stack
    // opaque layers (sclera, iris, pupil) and alpha-blended ones (cornea,
    // shine, wetness shells).
    struct EyeLayer
    {
        bool mAlpha = false;
        std::vector<LLVector3> mVertices;
    };
    struct EyeVertices
    {
        std::vector<EyeLayer> mLayers;
    };

    // Skinned (current pose, agent space) vertices of the mesh eyeballs:
    // for each eye joint, the vertices of the avatar's rigged world
    // attachments weighted at least EYE_WEIGHT to it. Eyeballs rotate with
    // their joint, so they are skinned to it alone; lids, lashes and skin
    // follow other bones. Faces with alpha 0 are skipped (how head HUDs hide
    // alternate eyes). Only faces holding eye vertices are skinned.
    void collectEyeVertices(LLVOAvatar* avatar, LLJoint* const joints[EYE_JOINTS],
                            EyeVertices vertices[EYE_JOINTS], S32& prims, S32& faces)
    {
        auto collect = [&](LLViewerObject* object)
        {
            if (!object || object->isDead() || object->getPCode() != LL_PCODE_VOLUME)
            {
                return;
            }
            LLVOVolume* vobj = static_cast<LLVOVolume*>(object);
            if (!vobj->mDrawable || vobj->mDrawable->isDead() ||
                !vobj->mDrawable->isState(LLDrawable::RIGGED))
            {
                return;
            }
            const LLMeshSkinInfo* skin = vobj->getSkinInfo();
            const LLVolume* volume = vobj->getVolume();
            if (!skin || !volume)
            {
                return;
            }
            // Mesh joint index -> eye joint (0..3), or -1.
            std::vector<S32> eye_of(skin->mJointNames.size(), -1);
            bool any = false;
            for (size_t k = 0; k < skin->mJointNames.size(); ++k)
            {
                LLJoint* joint = avatar->getJoint(skin->mJointNames[k]);
                for (S32 e = 0; joint && e < EYE_JOINTS; ++e)
                {
                    if (joint == joints[e])
                    {
                        eye_of[k] = e;
                        any = true;
                    }
                }
            }
            if (!any)
            {
                return;
            }
            ++prims;
            for (S32 i = 0; i < volume->getNumVolumeFaces(); ++i)
            {
                const LLTextureEntry* te = vobj->getTE((U8)i);
                const LLVolumeFace& face = volume->getVolumeFace(i);
                if ((te && te->getColor().mV[3] == 0.f) || !face.mWeights)
                {
                    continue;
                }
                const LLFace* drawn = i < vobj->mDrawable->getNumFaces() ? vobj->mDrawable->getFace(i) : nullptr;
                const bool alpha = drawn && drawn->isInAlphaPool();
                const LLVolumeFace* skinned = nullptr;
                S32 layer[EYE_JOINTS] = { -1, -1, -1, -1 }; // this face's layer per eye
                for (S32 j = 0; j < face.mNumVertices; ++j)
                {
                    // Per influence: integer part = mesh joint index,
                    // fraction = weight (as LLSkinningUtil decodes it).
                    const F32* w = face.mWeights[j].getF32ptr();
                    F32 total = 0.f;
                    F32 eye_weight[EYE_JOINTS] = { 0.f, 0.f, 0.f, 0.f };
                    for (S32 k = 0; k < 4; ++k)
                    {
                        const F32 whole = floorf(w[k]);
                        const S32 index = (S32)whole;
                        const F32 weight = w[k] - whole;
                        total += weight;
                        if (index >= 0 && index < (S32)eye_of.size() && eye_of[index] >= 0)
                        {
                            eye_weight[eye_of[index]] += weight;
                        }
                    }
                    for (S32 e = 0; total > 0.f && e < EYE_JOINTS; ++e)
                    {
                        if (eye_weight[e] < EYE_WEIGHT * total)
                        {
                            continue;
                        }
                        if (!skinned)
                        { // Skins this face only, no octree.
                            vobj->updateRiggedVolume(true, i, false);
                            const LLRiggedVolume* rigged = vobj->getRiggedVolume();
                            if (!rigged || i >= rigged->getNumVolumeFaces())
                            {
                                return;
                            }
                            skinned = &rigged->getVolumeFace(i);
                            ++faces;
                        }
                        if (j < skinned->mNumVertices)
                        {
                            std::vector<EyeLayer>& layers = vertices[e].mLayers;
                            if (layer[e] < 0)
                            {
                                layer[e] = (S32)layers.size();
                                layers.emplace_back();
                                layers.back().mAlpha = alpha;
                            }
                            layers[layer[e]].mVertices.emplace_back(skinned->mPositions[j].getF32ptr());
                        }
                    }
                }
            }
        };

        for (const auto& entry : avatar->mAttachmentPoints)
        {
            LLViewerJointAttachment* attachment = entry.second;
            if (!attachment || attachment->getIsHUDAttachment())
            {
                continue;
            }
            for (const auto& root : attachment->mAttachedObjects)
            {
                if (!root)
                {
                    continue;
                }
                collect(root.get());
                for (const LLPointer<LLViewerObject>& child : root->getChildren())
                {
                    collect(child.get());
                }
            }
        }
    }

    // Measured eyeballs of an avatar, per side (0 left, 1 right), in their
    // joint's frame, so they follow eye rotation.
    struct EyeShape
    {
        bool mMeasured = false;
        bool mAlt = false;               // eyeballs skinned to mFaceEyeAlt*
        bool mSide[2] = { false, false };
        std::vector<LLVector3> mLocal[2]; // eyeball vertices
        LLVector3 mCentre[2];             // their bounding-box centre
        S32 mTries = 0;
        F64 mNextTry = 0.0;
    };
    std::map<LLUUID, EyeShape> sEyeShapes;

    // Measures the avatar's mesh eyeballs: the joint set (mEye* or
    // mFaceEyeAlt*) holding more eyeball vertices; per eye, the vertices of
    // its opaque layers (sclera, iris, pupil: where a photographer
    // focuses), or of all its layers when the opaque ones hold too few,
    // stored in the joint frame. No shape is assumed: eyesPosition()
    // focuses on the stored vertex nearest the camera, so alpha-blended
    // shells (cornea, shine, wetness) in front of the iris are ignored.
    // Logged per layer: vertices, opaque or alpha, extents (about 24 mm on
    // each axis for a whole eyeball), centre's distance from the joint and
    // front's distance before the joint toward the camera. False without
    // eyeball vertices (system eyes, meshes not loaded, eyes not rigged to
    // eye joints).
    bool measureEyes(LLVOAvatar* avatar, EyeShape& entry)
    {
        LLJoint* const joints[EYE_JOINTS] = {
            avatar->mEyeLeftp, avatar->mEyeRightp,
            avatar->getJoint("mFaceEyeAltLeft"), avatar->getJoint("mFaceEyeAltRight") };
        EyeVertices vertices[EYE_JOINTS];
        S32 prims = 0;
        S32 faces = 0;
        collectEyeVertices(avatar, joints, vertices, prims, faces);

        auto count = [&](S32 e, bool opaque_only)
        {
            size_t n = 0;
            for (const EyeLayer& layer : vertices[e].mLayers)
            {
                n += opaque_only && layer.mAlpha ? 0 : layer.mVertices.size();
            }
            return n;
        };
        const bool alt = count(2, false) + count(3, false) > count(0, false) + count(1, false);
        const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
        std::string log;
        bool any = false;
        for (S32 side = 0; side < 2; ++side)
        {
            const S32 e = (alt ? 2 : 0) + side;
            entry.mSide[side] = false;
            entry.mLocal[side].clear();
            if (!joints[e] || count(e, false) < MIN_EYE_VERTICES)
            {
                continue;
            }
            const bool opaque_only = count(e, true) >= MIN_EYE_VERTICES;
            const LLVector3 joint_pos = joints[e]->getWorldPosition();
            const LLQuaternion to_local = ~joints[e]->getWorldRotation();
            const LLVector3 at_local = at * to_local;
            std::vector<LLVector3>& local = entry.mLocal[side];
            LLVector3 used_lo(F32_MAX, F32_MAX, F32_MAX);
            LLVector3 used_hi(-F32_MAX, -F32_MAX, -F32_MAX);
            log += llformat(" %s (%s layers used):", joints[e]->getName().c_str(),
                            opaque_only ? "opaque" : "all");
            for (const EyeLayer& layer : vertices[e].mLayers)
            {
                const bool used = !(opaque_only && layer.mAlpha);
                LLVector3 lo(F32_MAX, F32_MAX, F32_MAX);
                LLVector3 hi(-F32_MAX, -F32_MAX, -F32_MAX);
                F32 front = F32_MAX;
                for (const LLVector3& v : layer.mVertices)
                {
                    const LLVector3 l = (v - joint_pos) * to_local;
                    lo.set(llmin(lo.mV[VX], l.mV[VX]), llmin(lo.mV[VY], l.mV[VY]), llmin(lo.mV[VZ], l.mV[VZ]));
                    hi.set(llmax(hi.mV[VX], l.mV[VX]), llmax(hi.mV[VY], l.mV[VY]), llmax(hi.mV[VZ], l.mV[VZ]));
                    front = llmin(front, l * at_local);
                    if (used)
                    {
                        local.push_back(l);
                    }
                }
                if (used)
                {
                    used_lo.set(llmin(used_lo.mV[VX], lo.mV[VX]), llmin(used_lo.mV[VY], lo.mV[VY]), llmin(used_lo.mV[VZ], lo.mV[VZ]));
                    used_hi.set(llmax(used_hi.mV[VX], hi.mV[VX]), llmax(used_hi.mV[VY], hi.mV[VY]), llmax(used_hi.mV[VZ], hi.mV[VZ]));
                }
                const LLVector3 size = (hi - lo) * 1000.f;
                log += llformat(" [%d %s%s, %.1f x %.1f x %.1f mm, centre %.1f mm from joint, front %.1f mm]",
                                (S32)layer.mVertices.size(), layer.mAlpha ? "alpha" : "opaque",
                                used ? "" : " ignored", size.mV[VX], size.mV[VY], size.mV[VZ],
                                ((lo + hi) * 0.5f).length() * 1000.f, -front * 1000.f);
            }
            log += ";";
            if (local.size() < MIN_EYE_VERTICES)
            {
                local.clear();
                continue;
            }
            entry.mCentre[side] = (used_lo + used_hi) * 0.5f;
            entry.mSide[side] = true;
            any = true;
        }
        LL_INFOS("ASDoFAutofocus") << "Eyeball measure " << avatar->getFullname() << ": "
                                   << prims << " prims rigged to eye joints, " << faces << " eyeball faces;"
                                   << (log.empty() ? std::string(" no eyeball vertices") : log) << LL_ENDL;
        entry.mAlt = alt;
        return any;
    }

    // Eyeball measurement of an avatar: measured once, the first time the
    // avatar is focused on, and cached (an automatic re-measure could
    // shift the focus slightly and restart a converging aperture-sampled
    // image). Failures (meshes loading) retry every RETRY_SECONDS,
    // MAX_TRIES times. The floater's "Redetect"
    // (ASDepthOfField.RedetectEyes) clears the cache after an outfit
    // change. Snapshots and captures never measure (update() holds before).
    const EyeShape& eyeInfo(LLVOAvatar* avatar)
    {
        if (sEyeShapes.size() > 64)
        {
            sEyeShapes.clear();
        }
        EyeShape& entry = sEyeShapes[avatar->getID()];
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (entry.mTries < MAX_TRIES && now >= entry.mNextTry)
        {
            if (measureEyes(avatar, entry))
            {
                entry.mMeasured = true;
                entry.mTries = MAX_TRIES; // done until "Redetect"
            }
            else
            {
                ++entry.mTries;
                entry.mNextTry = now + RETRY_SECONDS;
            }
        }
        return entry;
    }

    // Overlay label: how the eye focus point is found for an avatar.
    LLVector3 eyesPosition(LLVOAvatar* avatar, bool measured);
    LLVOAvatar* findAvatar(const LLUUID& id);

    std::string eyeRadiusLabel(const LLUUID& id)
    {
        static LLCachedControl<F32> manual_mm(gSavedSettings, "ASDepthOfFieldAutofocusEyeRadius", 0.f);
        if (manual_mm > 0.f)
        {
            return llformat("eye %.1f mm set", llmin((F32)manual_mm, 50.f));
        }
        auto it = sEyeShapes.find(id);
        LLVOAvatar* avatar = findAvatar(id);
        if (it != sEyeShapes.end() && it->second.mMeasured && avatar)
        { // Eyeball centre (joint) to the focus point: the iris front.
            LLJoint* eye = nearerEye(eyePair(avatar, it->second.mAlt));
            return llformat("eye %.1f mm measured",
                            dist_vec(eyesPosition(avatar, true), eye->getWorldPosition()) * 1000.f);
        }
        return llformat("eye %.1f mm default", EYEBALL_RADIUS * 1000.f);
    }

    // Focus point on an avatar's eyes, on the nearer eye. Measured
    // (eyeInfo()): the eyeball vertex nearest the camera along the view
    // axis (the iris or sclera facing the camera), or with
    // ASDepthOfFieldAutofocusEyeRadius the eyeball centre plus that radius
    // toward the camera. Not measured, or `measured` false (enough to rank
    // candidates): the joint plus EYEBALL_RADIUS (or the set radius).
    LLVector3 eyesPosition(LLVOAvatar* avatar, bool measured)
    {
        static LLCachedControl<F32> manual_mm(gSavedSettings, "ASDepthOfFieldAutofocusEyeRadius", 0.f);
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const EyeShape* info = measured ? &eyeInfo(avatar) : nullptr;
        const EyePair pair = eyePair(avatar, info && info->mAlt);
        LLJoint* eye = nearerEye(pair);
        const S32 side = eye == pair.mLeft ? 0 : 1;
        const LLVector3 joint_pos = eye->getWorldPosition();
        const bool shape = info && info->mSide[side];

        if (shape && manual_mm <= 0.f)
        {
            const LLQuaternion rotation = eye->getWorldRotation();
            const LLVector3 at_local = camera->getAtAxis() * ~rotation;
            const LLVector3* front = nullptr;
            F32 nearest = F32_MAX;
            for (const LLVector3& l : info->mLocal[side])
            {
                const F32 depth = l * at_local;
                if (depth < nearest)
                {
                    nearest = depth;
                    front = &l;
                }
            }
            if (front)
            {
                return joint_pos + *front * rotation;
            }
        }

        const LLVector3 centre = shape ? joint_pos + info->mCentre[side] * eye->getWorldRotation() : joint_pos;
        const F32 radius = measured && manual_mm > 0.f ? llmin((F32)manual_mm, 50.f) * 0.001f : EYEBALL_RADIUS;
        LLVector3 towards = camera->getOrigin() - centre;
        towards.normVec();
        return centre + towards * radius;
    }

    LLVOAvatar* findAvatar(const LLUUID& id)
    {
        LLViewerObject* object = gObjectList.findObject(id);
        LLVOAvatar* avatar = object && !object->isDead() ? object->asAvatar() : nullptr;
        return avatar && avatar->mEyeLeftp && avatar->mEyeRightp ? avatar : nullptr;
    }

    // Visible avatar whose eyes are inside the area, nearest the camera:
    // in a crowd, the subject in front, not people behind it (one whose
    // eyes are hidden, e.g. seen from behind, fails the occlusion probe
    // and the area autofocus takes over).
    bool findEyes(const LLVector4& rect, LLVector2& eye_uv, F32& eye_distance, LLUUID& avatar_id)
    {
        F32 best_distance = F32_MAX;
        LLVOAvatar* best = nullptr;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead() || avatar->isControlAvatar() ||
                !avatar->mEyeLeftp || !avatar->mEyeRightp ||
                !avatar->mDrawable || !avatar->mDrawable->isVisible() ||
                (avatar->isSelf() && gAgentCamera.cameraMouselook()))
            {
                continue;
            }
            LLVector2 uv;
            F32 distance;
            if (!projectToUV(eyesPosition(avatar, false), uv, distance) || !insideRect(rect, uv))
            {
                continue;
            }
            if (distance < best_distance)
            {
                best_distance = distance;
                best = avatar;
            }
        }
        // Only the chosen avatar's eyeball is measured.
        if (!best || !projectToUV(eyesPosition(best, true), eye_uv, eye_distance))
        {
            return false;
        }
        avatar_id = best->getID();
        return true;
    }

    void deleteFence(Slot& slot)
    {
        if (slot.mFence)
        {
            glDeleteSync(slot.mFence);
            slot.mFence = 0;
        }
    }

    void dropReadbacks()
    {
        for (Slot& slot : sSlots)
        {
            deleteFence(slot);
        }
    }

    // Drops readbacks in flight and the focus state.
    void reset()
    {
        dropReadbacks();
        sSubject = Subject();
        sLockHandled = false;
        sSubjectInView = false;
        sHasTarget = false;
        sHasFocus = false;
        sMoving = false;
        sEyesTracked = false;
        sHasEyeCandidate = false;
    }

    // 1/distance-weighted quantile of the area samples.
    F32 weightedQuantile(F32 quantile)
    {
        const F32 far_distance = llmax(LLViewerCamera::getInstance()->getFar(), 1.f);
        sSorted.resize(SAMPLE_COUNT);
        for (S32 i = 0; i < SAMPLE_COUNT; ++i)
        {
            const F32 z = sReadback[i];
            sSorted[i] = std::isfinite(z) ? llclamp(z, MIN_DISTANCE, far_distance) : far_distance;
        }
        std::sort(sSorted.begin(), sSorted.end());

        F32 total = 0.f;
        for (F32 z : sSorted)
        {
            total += 1.f / z;
        }
        const F32 goal = quantile * total;
        F32 running = 0.f;
        for (S32 i = 0; i < SAMPLE_COUNT; ++i)
        {
            const F32 w = 1.f / sSorted[i];
            if (running + w >= goal)
            {
                const F32 t = llclamp((goal - running) / w, 0.f, 1.f);
                return i > 0 ? lerp(sSorted[i - 1], sSorted[i], t) : sSorted[i];
            }
            running += w;
        }
        return sSorted.back();
    }

    // Reads every finished readback; the newest one sets the target.
    void collect()
    {
        static LLCachedControl<F32> near_priority(gSavedSettings, "ASDepthOfFieldAutofocusNearPriority", 0.f);
        Slot* newest = nullptr;
        for (Slot& slot : sSlots)
        {
            if (!slot.mFence)
            {
                continue;
            }
            const GLenum status = glClientWaitSync(slot.mFence, 0, 0);
            if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
            {
                if (status == GL_WAIT_FAILED)
                {
                    deleteFence(slot);
                }
                continue;
            }
            deleteFence(slot);
            if (!newest || slot.mSequence > newest->mSequence)
            {
                newest = &slot;
            }
        }
        if (!newest)
        {
            return;
        }

        sReadback.resize(READBACK_FLOATS);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, newest->mBuffer);
        glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, READBACK_FLOATS * sizeof(F32), sReadback.data());
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        sEyesTracked = false;
        if (newest->mHasEyes)
        { // Probe texel: scene distance at the eyes.
            const F32 probe = sReadback[SAMPLE_COUNT];
            if (!std::isfinite(probe) ||
                probe >= newest->mEyeDistance * EYE_VISIBLE_RATIO - EYE_VISIBLE_MARGIN)
            {
                sTargetDistance = newest->mEyeDistance;
                sEyesTracked = true;
                sEyesAvatar = newest->mEyeAvatar;
            }
        }
        if (!sEyesTracked)
        {
            const F32 quantile = 0.5f - 0.45f * llclamp((F32)near_priority, 0.f, 1.f);
            sTargetDistance = weightedQuantile(quantile);
        }
        sHasTarget = true;
    }

    // Renders this frame's samples and starts their readback.
    void issue(LLRenderTarget& depth, LLVertexBuffer& triangle)
    {
        Slot* slot = nullptr;
        for (Slot& candidate : sSlots)
        {
            if (!candidate.mFence)
            {
                slot = &candidate;
                break;
            }
        }
        if (!slot)
        {
            return;
        }
        if (!sTarget.isComplete() && !sTarget.allocate(GRID_W, TARGET_H, GL_R32F))
        {
            LL_WARNS_ONCE("ASDoFAutofocus") << "Autofocus target allocation failed" << LL_ENDL;
            return;
        }
        if (!slot->mBuffer)
        {
            glGenBuffers(1, &slot->mBuffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, slot->mBuffer);
            glBufferData(GL_PIXEL_PACK_BUFFER, READBACK_FLOATS * sizeof(F32), nullptr, GL_STREAM_READ);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }

        const LLVector4 rect = areaRect();
        LLVector2 eye_uv(0.5f, 0.5f);
        F32 eye_distance = 0.f;
        slot->mHasEyes = focusMode() == FOCUS_EYES &&
                         findEyes(rect, eye_uv, eye_distance, slot->mEyeAvatar);
        slot->mEyeDistance = eye_distance;
        sHasEyeCandidate = slot->mHasEyes;
        sEyeUV = eye_uv;

        LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        sTarget.bindTarget();
        sProgram.bind();
        sProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth, true, LLTexUnit::TFO_POINT);
        sProgram.uniform4f(U_AF_RECT, rect.mV[0], rect.mV[1], rect.mV[2], rect.mV[3]);
        sProgram.uniform2f(U_EYE_UV, eye_uv.mV[VX], eye_uv.mV[VY]);
        triangle.setBuffer();
        triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
        sProgram.unbind();

        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot->mBuffer);
        glReadPixels(0, 0, GRID_W, TARGET_H, GL_RED, GL_FLOAT, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        slot->mFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        slot->mSequence = ++sSequence;
        sTarget.flush();
    }

    // Moves sCurrent toward the target in 1/distance, with a dead band.
    void smooth()
    {
        static LLCachedControl<F32> focus_time(gSavedSettings, "ASDepthOfFieldAutofocusTime", 0.5f);
        const F32 relative = fabsf(sTargetDistance - sCurrent) / sCurrent;
        if (!sMoving && relative > START_BAND)
        {
            sMoving = true;
        }
        if (!sMoving)
        {
            return;
        }
        const F32 dt = llclamp(gFrameIntervalSeconds.value(), 0.f, 0.25f);
        const F32 time = llmax((F32)focus_time, 0.f);
        // Reaches 99% of the way in `time` seconds at any frame rate.
        const F32 alpha = time > 0.001f ? 1.f - powf(0.01f, dt / time) : 1.f;
        sCurrent = 1.f / lerp(1.f / sCurrent, 1.f / sTargetDistance, alpha);
        if (fabsf(sTargetDistance - sCurrent) / sTargetDistance < SETTLE_BAND)
        {
            sCurrent = sTargetDistance;
            sMoving = false;
        }
    }

    // Subject for a new focus lock: the tracked eyes in eye mode, otherwise
    // the surface under the area centre. Avatar surfaces (body, rigged or
    // attached meshes) follow their nearest skeleton joint, so a mesh face
    // follows the head; other objects follow their own position and
    // rotation. Nothing hit leaves NONE (the distance freezes).
    void acquireSubject(const LLVector4& rect)
    {
        sSubject = Subject();
        if (focusMode() == FOCUS_EYES && sEyesTracked)
        {
            if (LLVOAvatar* avatar = findAvatar(sEyesAvatar))
            {
                sSubject.mType = Subject::EYES;
                sSubject.mID = avatar->getID();
                sSubject.mLabel = avatar->getFullname();
                { // Focus diagnostics: view-axis distances (mm).
                    const LLViewerCamera* camera = LLViewerCamera::getInstance();
                    const LLVector3 point = eyesPosition(avatar, true);
                    const EyeShape& info = eyeInfo(avatar);
                    LLJoint* eye = nearerEye(eyePair(avatar, info.mAlt));
                    LL_INFOS("ASDoFAutofocus") << "Eye lock " << sSubject.mLabel << ": " << eye->getName()
                        << " joint at " << (eye->getWorldPosition() - camera->getOrigin()) * camera->getAtAxis() * 1000.f
                        << " mm, focus point at " << (point - camera->getOrigin()) * camera->getAtAxis() * 1000.f
                        << " mm, current focus " << sCurrent * 1000.f << " mm ("
                        << eyeRadiusLabel(sSubject.mID) << ")" << LL_ENDL;
                }
                return;
            }
        }

        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const F32 tan_half = tanf(camera->getView() * 0.5f);
        const F32 x = (rect.mV[0] + rect.mV[2]) - 1.f; // -1..1
        const F32 y = (rect.mV[1] + rect.mV[3]) - 1.f;
        LLVector3 direction = camera->getAtAxis()
            - camera->getLeftAxis() * (x * tan_half * camera->getAspect())
            + camera->getUpAxis() * (y * tan_half);
        direction.normVec();
        LLVector4a start;
        LLVector4a end;
        LLVector4a hit;
        start.load3((camera->getOrigin() + direction * camera->getNear()).mV);
        end.load3((camera->getOrigin() + direction * 512.f).mV);
        S32 face = -1;
        LLViewerObject* object = gPipeline.lineSegmentIntersectInWorld(start, end,
            false, true, true, false, &face, nullptr, nullptr, &hit);
        if (!object || object->isDead())
        {
            return;
        }
        const LLVector3 point(hit.getF32ptr());

        LLVOAvatar* avatar = object->asAvatar() ? object->asAvatar() : object->getAvatar();
        if (avatar && !avatar->isDead())
        {
            const LLAvatarAppearance::avatar_joint_list_t& skeleton = avatar->getSkeleton();
            S32 best = -1;
            F32 best_distance = F32_MAX;
            for (S32 i = 0; i < (S32)skeleton.size(); ++i)
            {
                if (!skeleton[i])
                {
                    continue;
                }
                const F32 d = dist_vec_squared(skeleton[i]->getWorldPosition(), point);
                if (d < best_distance)
                {
                    best_distance = d;
                    best = i;
                }
            }
            if (best >= 0)
            {
                LLJoint* joint = skeleton[best];
                sSubject.mType = Subject::JOINT;
                sSubject.mID = avatar->getID();
                sSubject.mJoint = best;
                sSubject.mOffset = (point - joint->getWorldPosition()) * ~joint->getWorldRotation();
                sSubject.mLabel = avatar->getFullname();
                return;
            }
        }

        sSubject.mType = Subject::OBJECT;
        sSubject.mID = object->getID();
        sSubject.mOffset = (point - object->getPositionAgent()) * ~object->getRotationRegion();
    }

    // Current agent-space position of the locked subject; false when it
    // is gone.
    bool subjectPosition(LLVector3& position)
    {
        LLViewerObject* object = gObjectList.findObject(sSubject.mID);
        if (!object || object->isDead())
        {
            return false;
        }
        switch (sSubject.mType)
        {
        case Subject::EYES:
            if (LLVOAvatar* avatar = findAvatar(sSubject.mID))
            {
                position = eyesPosition(avatar, true);
                return true;
            }
            return false;
        case Subject::JOINT:
            if (LLVOAvatar* avatar = object->asAvatar())
            {
                const LLAvatarAppearance::avatar_joint_list_t& skeleton = avatar->getSkeleton();
                if (sSubject.mJoint < (S32)skeleton.size() && skeleton[sSubject.mJoint])
                {
                    LLJoint* joint = skeleton[sSubject.mJoint];
                    position = joint->getWorldPosition() + sSubject.mOffset * joint->getWorldRotation();
                    return true;
                }
            }
            return false;
        case Subject::OBJECT:
            position = object->getPositionAgent() + sSubject.mOffset * object->getRotationRegion();
            return true;
        default:
            return false;
        }
    }

    // Focus lock in autofocus. Returns true when the lock decided this
    // frame's focus (sCurrent), false when it is off.
    bool updateLock(const LLVector4& rect)
    {
        static LLCachedControl<S32> lock_mode(gSavedSettings, "ASDepthOfFieldAutofocusLockMode", LOCK_SUBJECT);
        static LLCachedControl<bool> track_outside(gSavedSettings, "ASDepthOfFieldAutofocusTrackOutside", true);
        sSubjectInView = false;
        if (!LLPipeline::FSFocusPointLocked)
        {
            if (sLockHandled)
            {
                sLockHandled = false;
                sSubject = Subject();
            }
            return false;
        }
        if (!sHasFocus)
        { // Locked before autofocus had a result (e.g. locked in point
          // mode, then autofocus selected): run until there is one.
            return false;
        }
        if (!sLockHandled)
        { // New lock: older readbacks no longer matter.
            sLockHandled = true;
            dropReadbacks();
            if (lock_mode == LOCK_SUBJECT)
            {
                acquireSubject(rect);
            }
        }

        LLVector3 position;
        LLVector2 uv;
        F32 distance;
        if (sSubject.mType != Subject::NONE &&
            subjectPosition(position) && projectToUV(position, uv, distance) &&
            (track_outside || insideRect(rect, uv)))
        {
            sTargetDistance = distance;
            sSubjectInView = true;
            sSubjectUV = uv;
            smooth();
        }
        // Otherwise the distance holds: classic lock, subject gone, behind
        // the camera, or outside the area without TrackOutside.
        return true;
    }
}

void ASDoFAutofocus::registerUICallbacks()
{
    // Menu toggle (Alt+Shift+Z): point focus <-> the last autofocus mode.
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASDepthOfField.ToggleAutofocus",
        [](LLUICtrl*, const LLSD&)
        {
            const S32 mode = gSavedSettings.getS32("ASDepthOfFieldFocusMode");
            if (mode != FOCUS_POINT)
            {
                gSavedSettings.setS32("ASDepthOfFieldAutofocusLastMode", mode);
                gSavedSettings.setS32("ASDepthOfFieldFocusMode", FOCUS_POINT);
            }
            else
            {
                const S32 last = gSavedSettings.getS32("ASDepthOfFieldAutofocusLastMode");
                gSavedSettings.setS32("ASDepthOfFieldFocusMode", last == FOCUS_EYES ? FOCUS_EYES : FOCUS_AREA);
            }
        });
    // Floater "Redetect": measure every avatar's eyeballs again.
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add(
        "ASDepthOfField.RedetectEyes",
        [](LLUICtrl*, const LLSD&)
        {
            sEyeShapes.clear();
        });
    LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add(
        "ASDepthOfField.IsAutofocus",
        [](LLUICtrl*, const LLSD&)
        {
            return focusMode() != FOCUS_POINT;
        });
}

bool ASDoFAutofocus::isActive()
{
    static LLCachedControl<bool> dof_enabled(gSavedSettings, "RenderDepthOfField", false);
    return dof_enabled && focusMode() != FOCUS_POINT && sProgram.isComplete();
}

void ASDoFAutofocus::registerShaders(std::vector<LLGLSLShader*>& shaders)
{
    shaders.push_back(&sProgram);
}

bool ASDoFAutofocus::createShaders(S32 shader_level)
{
    sProgram.mName = "AyaneStorm Depth of Field Autofocus Shader";
    sProgram.mShaderFiles.clear();
    sProgram.clearPermutations();
    sProgram.mFeatures.isDeferred = true;
    sProgram.mShaderFiles.emplace_back("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER);
    sProgram.mShaderFiles.emplace_back("deferred/asDoFAutofocusF.glsl", GL_FRAGMENT_SHADER);
    sProgram.mShaderLevel = shader_level;
    return sProgram.createShader();
}

void ASDoFAutofocus::unloadShaders()
{
    sProgram.unload();
    releaseResources();
}

void ASDoFAutofocus::releaseResources()
{
    reset();
    for (Slot& slot : sSlots)
    {
        if (slot.mBuffer)
        {
            glDeleteBuffers(1, &slot.mBuffer);
            slot.mBuffer = 0;
        }
    }
    sTarget.release();
}

bool ASDoFAutofocus::update(LLRenderTarget& depth, LLVertexBuffer& triangle, F32& distance)
{
    if (focusMode() == FOCUS_POINT || !sProgram.isComplete())
    {
        if (sHasTarget || sHasFocus)
        {
            reset();
        }
        return false;
    }

    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame > sLastFrame + STALE_FRAMES)
    {
        reset();
    }
    sLastFrame = frame;

    // Snapshots and sliced captures keep the live focus.
    if (gSnapshot || ASDoFRenderer::isWorldFrozen())
    {
        if (sHasFocus)
        {
            distance = sCurrent;
            return true;
        }
        return false;
    }

    // Focus lock: tracks the subject or holds the distance.
    if (updateLock(areaRect()))
    {
        distance = sCurrent;
        return true;
    }

    collect();
    issue(depth, triangle);
    if (!sHasTarget)
    {
        return false;
    }
    if (!sHasFocus)
    {
        sCurrent = distance > MIN_DISTANCE ? distance : sTargetDistance;
        sHasFocus = true;
    }
    smooth();
    distance = sCurrent;
    return true;
}

void ASDoFAutofocus::drawOverlay()
{
    static LLCachedControl<bool> show_area(gSavedSettings, "ASDepthOfFieldAutofocusShowArea", true);
    // Firestorm's "Draw DoF Focus crosshair" shows this helper instead.
    static LLCachedControl<bool> show_crosshair(gSavedSettings, "FSFocusPointRender", false);
    // Hidden under the teleport/login progress screen and with the UI
    // hidden (Ctrl+Alt+F1), like Firestorm's focus crosshair.
    LLProgressView* progress = gViewerWindow->getProgressView();
    if (gSnapshot || !isActive() || !(show_crosshair || show_area) ||
        (progress && progress->getVisible()) ||
        !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        return;
    }

    const LLVector4 rect = areaRect();
    const bool locked = LLPipeline::FSFocusPointLocked && sLockHandled;
    // Red locked, green on eyes, yellow on the area.
    const LLColor4 colour = locked ? LLColor4(1.f, 0.3f, 0.25f, 0.9f)
                          : sEyesTracked ? LLColor4(0.3f, 1.f, 0.4f, 0.9f)
                          : LLColor4(1.f, 0.85f, 0.1f, 0.9f);

    // Drawn before HUD and UI (see header): enter 2D state here and restore
    // the 3D matrices and viewport the HUD elements render with.
    const S32 saved_viewport[4] = { gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3] };
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gViewerWindow->setup2DRender();
    LLGLSUIDefault gls_ui;
    LLGLDepthTest no_depth(GL_FALSE, GL_FALSE);

    // Same space as the viewer's debug text: scaled UI coordinates.
    gUIProgram.bind();
    gGL.pushMatrix();
    gGL.pushUIMatrix();
    {
        const LLVector2& scale = gViewerWindow->getDisplayScale();
        gGL.scaleUI(scale.mV[VX], scale.mV[VY], 1.f);

        const LLRect view = gViewerWindow->getWorldViewRectScaled();
        auto to_x = [&](F32 u) { return view.mLeft + ll_round(u * view.getWidth()); };
        auto to_y = [&](F32 v) { return view.mBottom + ll_round(v * view.getHeight()); };

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        const S32 left = to_x(rect.mV[0]);
        const S32 top = to_y(rect.mV[3]);
        gl_rect_2d(left, top, to_x(rect.mV[2]), to_y(rect.mV[1]), colour, false);
        if (locked ? sSubjectInView : sHasEyeCandidate)
        { // Tracked subject or eyes.
            const LLVector2& uv = locked ? sSubjectUV : sEyeUV;
            const S32 x = to_x(uv.mV[VX]);
            const S32 y = to_y(uv.mV[VY]);
            gl_rect_2d(x - 6, y + 4, x + 6, y - 4, colour, false);
        }

        if (sHasFocus)
        {
            std::string text = llformat("AF %.2f m", sCurrent);
            if (locked)
            {
                if (sSubject.mType == Subject::NONE)
                {
                    text += " (locked)";
                }
                else
                {
                    const std::string& name = sSubject.mLabel.empty() ? std::string("object") : sSubject.mLabel;
                    const std::string eyes = sSubject.mType == Subject::EYES
                        ? ", " + eyeRadiusLabel(sSubject.mID) : std::string();
                    text += llformat(" (locked: %s%s%s)", name.c_str(), eyes.c_str(),
                                     sSubjectInView ? "" : ", holding");
                }
            }
            else if (sEyesTracked)
            {
                text += " (" + eyeRadiusLabel(sEyesAvatar) + ")";
            }
            LLFontGL::getFontSansSerif()->renderUTF8(text, 0, (F32)left, (F32)(top + 2), colour,
                LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        }
    }
    gGL.popUIMatrix();
    gGL.popMatrix();
    gGL.flush();
    gUIProgram.unbind();

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    for (S32 i = 0; i < 4; ++i)
    {
        gGLViewport[i] = saved_viewport[i];
    }
    glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
}
