/**
 * @file asravatarbody.h
 * @brief The system avatar body in the new renderer: head, upper and lower body, eyes,
 * eyelashes, hair and skirt, skinned and morphed on the GPU.
 *
 * Each body mesh (LLPolyMeshSharedData) is sent once for every avatar: a morph set of its
 * base vertices and morph targets, a skin, and a geometry per LOD over the same vertices.
 * An avatar adds a morph per mesh, whose weights are the viewer's applied ones, and an
 * instance per shown part, on its skeleton. The viewer's rules for which part shows, its
 * LOD and its texture are followed as LLVOAvatar::renderSkinned() has them. See
 * doc/ayanestorm-new-renderer-avatar-body-plan.md.
 * Author: chanayane@firestorm
 */
#ifndef ASR_AVATAR_BODY_H
#define ASR_AVATAR_BODY_H

#include "stdtypes.h"

#include <memory>

class LLAvatarJointMesh;
class LLPolyMesh;
class LLPolyMeshSharedData;
class LLViewerTexture;
class LLVOAvatar;
namespace asr
{
struct MaterialDesc;
}

// A class, not a namespace: LL's appearance classes name it as a friend.
class ASRAvatarBody
{
public:
    struct Body;
    struct BodyDeleter
    {
        void operator()(Body* body) const; // frees memory only: see release()
    };
    using BodyPtr = std::unique_ptr<Body, BodyDeleter>;

    // Whether the avatar has a system body to keep: alive, built, not animesh or a preview.
    static bool wanted(LLVOAvatar* avatar);

    // Brings the body in step with the avatar, drawn on skeleton. Makes it the first time.
    // Only what changed reaches the GPU. Morph weights are only read when posed: the
    // avatar's joints were read this frame, so it was animated or a sweep is due.
    static void update(LLVOAvatar* avatar, BodyPtr& body, U32 skeleton, bool posed);

    // Takes it off the GPU. Reads nothing of the avatar, which may be gone.
    static void release(BodyPtr& body);

    // Its morph masks were made again, in place.
    static void masksChanged(LLVOAvatar* avatar);

    // The renderer stops: forgets the shared meshes without touching the GPU.
    static void forget();

    // Temporary, for the log: since the last call.
    struct Counts
    {
        U32 bodies = 0;
        U32 parts = 0;
        U32 weightUpdates = 0;
        U32 maskUpdates = 0;
        U32 lodChanges = 0;
    };
    static Counts takeCounts();

    // Defined in the .cpp file.
    struct SharedMesh;
    struct BodyMorph;

private:
    // What reads LL's protected members: friendship reaches members only.
    static LLPolyMeshSharedData* referenceOf(LLPolyMeshSharedData* lod);
    static U32 makeMorphSet(LLVOAvatar* avatar, LLPolyMesh* mesh, SharedMesh& shared);
    static SharedMesh& sharedMesh(LLVOAvatar* avatar, LLPolyMesh* reference);
    static U32 lodGeometry(LLPolyMeshSharedData* lod, const SharedMesh& shared);
    static LLViewerTexture* partImage(LLVOAvatar* avatar, LLAvatarJointMesh* mesh, asr::MaterialDesc& desc);
    static BodyMorph& bodyMorph(Body& body, LLPolyMesh* reference, const SharedMesh& shared);
    static void sendMasks(BodyMorph& morph);
    static void updatePart(Body& body, S32 id, bool show);
};

#endif
