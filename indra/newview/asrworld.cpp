/**
 * @file asrworld.cpp
 * @brief See asrworld.h.
 */
#include "llviewerprecompiledheaders.h"

#include "asrworld.h"
#include "asravatarbody.h"
#include "asravatarresidency.h"
#include "asrskinweights.h"
#include "asrtexturecache.h"
#include "asrmeshlodresidency.h"
#include "asrtexturestreaming.h"
#include "asrworldshared.h"

#if defined(AS_RENDERER) && (LL_WINDOWS || LL_DARWIN) // as ASR_VIEWER_SUPPORTED in asrviewer.cpp

#include "asrbridge.h"
#include "asrcameraturn.h"
#include "asrdevice.h"
#include "asrrenderthread.h"
#include "asrviewer.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "fsavatarrenderpersistence.h"
#include "llviewerobjectlist.h"
#include "llappviewer.h"
#include "lldrawable.h"
#include "llfetchedgltfmaterial.h"
#include "llgltfmateriallist.h"
#include "llimage.h"
#include "llmaterial.h"
#include "llmeshrepository.h"
#include "llskinningutil.h"
#include "lltextureentry.h"
#include "lltoolpie.h"
#include "llcriticaldamp.h"
#include "llselectmgr.h"
#include "pipeline.h"
#include "rlvlocks.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

bool get_hud_matrices(glm::mat4& proj, glm::mat4& model); // llviewerdisplay.cpp
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvocache.h"
#include "llvolume.h"
#include "llvolumemgr.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewershadermgr.h"
#include "llviewertextureanim.h"
#include "llvlcomposition.h"
#include "llvosurfacepatch.h"
#include "llvovolume.h"
#include "llworld.h"
#include "noise.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Named outside the namespace below, so asrworldshared.h can hand it to asravatarbody.cpp.
struct ASRSharedMaterial;

namespace
{

constexpr U32 NO_INSTANCE = 0xFFFFFFFFu;
constexpr U32 LODS = LLVolumeLODGroup::NUM_LODS;
constexpr U32 MAX_WORKERS = 4;
constexpr auto LOG_PERIOD = std::chrono::seconds(20);
constexpr F32 LOD_SWEEP_SECONDS = 0.25f; // how long a pass over all objects takes

static_assert(LODS == asr::MAX_LODS, "the GPU walks every LOD of a shape");

// The viewer's LOD rule is (factor * radius) / distance against 0.03, 0.06 and 0.24. Here
// that is distance / radius from which each LOD gives way to the next coarser one, and
// the factor reaches the GPU through the camera's lodScale.
constexpr F32 COARSER_AT[LODS] = { 0.f, 1.f / 0.03f, 1.f / 0.06f, 1.f / 0.24f };

// One face of a shape: every LOD packed in one box.
struct ShapeFace
{
    U32 geometries[LODS] = { 0, 0, 0, 0 };
    U32 finest = 0; // 0 for a face with nothing to draw
    asr::GeometryBox box{};
    F32 extents[2][3]{}; // object space, as LLVolumeFace::mExtents: for HUD faces' draw order

    // Empty/coarse LODs do not invalidate another LOD's weighted surface.
    U32 skinJoints[LODS]{}; // highest weighted palette index + 1 per LOD; 0 without weights
};

// What makes geometry the same: a repeated mesh, a grass field say, is one shape however
// many objects show it. A mesh is its asset alone, as the profile and path an object also
// carries change nothing. A sculpt also depends on how sharp its map was.
using ShapeKey = std::pair<LLVolumeParams, S32>;

ShapeKey meshKey(const LLVolumeParams& params)
{
    LLVolumeParams asset;
    asset.setSculptID(params.getSculptID(), LL_SCULPT_TYPE_MESH);
    return ShapeKey(asset, 0);
}

bool isMeshKey(const ShapeKey& key)
{
    return (key.first.getSculptType() & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_MESH;
}

ShapeKey shapeKey(const LLVolume& volume)
{
    const LLVolumeParams& params = volume.getParams();
    const U8 type = params.getSculptType() & LL_SCULPT_TYPE_MASK;
    if (type == LL_SCULPT_TYPE_MESH)
    {
        return meshKey(params);
    }
    return ShapeKey(params, type == LL_SCULPT_TYPE_NONE ? 0 : volume.getSculptLevel());
}

struct Shape
{
    explicit Shape(const ShapeKey& shapeKey) : key(shapeKey) {}

    const ShapeKey key;
    std::vector<ShapeFace> faces; // the main thread's: a worker fills a copy
    LLVector3 lodBias{ 1.f, 1.f, 1.f };
    std::unordered_set<LLDrawable*> users;
    bool ready = false; // something has been built
    bool busy = false;  // a worker has it

    // A mesh's LODs come over the network one at a time, in any order. They are built
    // coarsest first, so the GPU's chain only ever grows at its fine end and never has a
    // hole, and whatever came too early waits here.
    U32 nextLod = 0;
    LLPointer<LLVolume> arrived[LODS];
    bool gone[LODS] = { false, false, false, false }; // the server has no such LOD
    asr::MeshLodResidency residency;
    S32 actualLods[LODS] = { -1, -1, -1, -1 }; // cached header remapping, including finer fallbacks
};

struct TerrainJob;

// One piece of work for a worker. A mesh job adds one LOD, any other job builds all of them.
// A terrain job builds a patch of ground instead, and has no shape.
struct ShapeJob
{
    std::shared_ptr<TerrainJob> terrain;
    Shape* shape = nullptr;
    std::vector<ShapeFace> faces; // what the shape had, and what it has after this job
    LLVector3 lodBias{ 1.f, 1.f, 1.f };
    LLPointer<LLVolume> mesh;
    U32 lod = 0;
    bool sculpted = false;
    LLPointer<LLImageRaw> sculptMap; // null when none is in memory
    bool sculptMissing = false;      // the asset does not exist, so it draws a placeholder
};

using SharedMaterial = ASRSharedMaterial;

// A texture some face shows. Its slot changes as sharper versions load.
struct TrackedTexture
{
    LLPointer<LLViewerTexture> image;
    U32 slot = 0;
    S8 components = 0;
    F32 visited = -1000.f; // see visitTextures()
    U32 changes = 0; // temporary: how often its slot changed
    struct Uses { U32 faces = 0; U32 eyes = 0; };
    std::unordered_map<LLDrawable*, Uses> users;
    U32 terrainUsers = 0;                       // layers of regions' ground, which follow its slot themselves
    std::vector<SharedMaterial*> materials;     // what a new slot has to be written to

    // Texture feedback: when it got its slot, as feedback from before then is another
    // texture's, and what the GPU last saw of it.
    asr::TextureFeedback::Clock::time_point slotSince{};
    U8 gpuLevel = 0;          // log2 of the texels across it needed, the largest lately: see takeFeedback()
    U8 latestGpuLevel = 0;    // immediate demand, without the retention window
    F32 gpuLevelAt = -1000.f; // when that level was last needed
    F32 gpuSeenAt = -1000.f;  // gFrameTimeSeconds
    U32 detailLimit = 0;      // longest side allowed for a small avatar detail
    F32 detailLimitAt = -1000.f;
};

// What makes two materials one. The slot is not part of it: it follows the image.
struct MaterialKey
{
    F32 values[17];
    U32 planar;
    const LLViewerTexture* image;
    asr::TextureAnimDesc anim;

    bool operator==(const MaterialKey& other) const { return memcmp(this, &other, sizeof(MaterialKey)) == 0; }
};
static_assert(sizeof(MaterialKey) == 136, "compared and hashed as bytes, so it has no padding");

} // namespace

struct ASRSharedMaterial
{
    U32 id = 0;
    U32 users = 0; // faces
    asr::MaterialDesc desc;
    TrackedTexture* texture = nullptr;
};

namespace
{

struct ObjectFace
{
    U32 instance = NO_INSTANCE;
    SharedMaterial* material = nullptr;
    U32 flags = 0;
    U32 rank = 0;        // see resolveStack()
    U32 tieRank = 0;     // see tieRankOf()
    bool hidden = false; // an exact copy of it is what shows
};

struct Stack;

// Which objects stand in exactly the same place with exactly the same shape.
struct StackKey
{
    const void* shape;
    S32 matrix[12];

    bool operator==(const StackKey& other) const { return memcmp(this, &other, sizeof(StackKey)) == 0; }
};
static_assert(sizeof(StackKey) == 56, "compared and hashed as bytes, so it has no padding");

// An avatar's skeleton on the GPU: its bones, collision volumes and attachment points, in
// the viewer's own joint numbering, which rigged meshes are made against.
struct AvatarSkeleton
{
    U32 id = 0;
    U32 joints = 0;
    std::vector<F32> rows; // as last posed
    bool settled = false;  // the last pose repeated the one before, so the GPU leads nothing
    std::unordered_set<LLDrawable*> users;
    F32 lodDistance = -1.f; // the avatar's distance when its objects' LODs were last checked
    S32 animationPeriod = 1; // every how many frames the viewer animates it: see setAnimationPeriods()
    bool interpolate = false; // drawn one period late, blended between poses, never overshooting
    U32 unchangedFrames = 0;  // since the last new pose
    bool animated = true;     // the viewer animated it since its joints were last read
    bool bodyPosed = true;    // accumulated until the lower-priority system body gets a turn
    LLVector3 origin;         // the render origin they were read with
    bool resident = true;     // its skinned copies kept: see setAnimationPeriods()
    double nearViewAt = 0.0;  // gFrameTimeSeconds when it was last near the view
    S32 lodCap = asr::AvatarDetailPolicy::FINEST; // the finest LOD its meshes may use: see setAnimationPeriods()
    ASRAvatarBody::BodyPtr body; // its system body, which keeps the skeleton as users do
};

// A rigged mesh's skin, shared by everything wearing that mesh.
using SkinKey = std::pair<LLUUID, U64>; // mesh, and the skin's hash
struct SharedSkin
{
    U32 id = 0;
    U32 joints = 0;
    U32 users = 0;
};

// An object drawn on an avatar: rigged to its skeleton, or attached to one of its joints.
struct AvatarBinding
{
    LLVOAvatar* avatar = nullptr;
    SharedSkin* skin = nullptr; // rigged
    SkinKey skinKey;
    U32 joint = 0;              // attached
    F32 local[12] = {};         // attached: its matrix in that joint's space, as last written
};

struct WorldObject
{
    std::unique_ptr<AvatarBinding> bound;
    Shape* shape = nullptr;
    Shape* next = nullptr; // what replaces shape once it is built, so nothing blinks out
    std::vector<ObjectFace> faces;
    Stack* stack = nullptr;
    StackKey stackKey{};
    size_t sweepSlot = 0;         // in sSweep
    F64 lodDue = F64_MAX;         // the camera travel at which its LOD is checked: see updateLods()
    S32 askedLod = -1;            // the finest LOD this object has asked the mesh system for
    bool askedWithHeader = false; // whether the mesh's header was known by then
    S32 skinnedLod = -1;          // rigged: the LOD its skinned faces were last applied at
};

struct SweepEntry
{
    LLDrawable* drawable;
    WorldObject* object;
};

struct LodCheck
{
    F64 due;
    LLDrawable* drawable; // may be gone: only compared, and looked up
    bool operator<(const LodCheck& other) const { return due > other.due; } // soonest on top
};

struct StackMember
{
    LLUUID id; // the order everyone agrees on, every session
    LLDrawable* drawable;
    WorldObject* object;
    bool rewrite;
};

struct Stack
{
    std::vector<StackMember> members;
};

template <typename Key>
struct BytesHash
{
    size_t operator()(const Key& key) const
    {
        return std::hash<std::string_view>()(std::string_view(reinterpret_cast<const char*>(&key), sizeof(Key)));
    }
};

asr::SceneQueue* sScene = nullptr;
bool sRunning = false;
asr::AvatarResidencyPolicy sAvatarResidency;
asr::AvatarDetailPolicy sAvatarDetail;

// Within this distance a surface is sharp to what the GPU asks for. It narrows while textures
// are over their limit and widens back once under: in a club nearly everything is within a
// fixed 20 m, and holding all of it sharp left nothing able to refine. See updateNearRange().
constexpr F32 NEAREST_RANGE = 4.f;
constexpr F32 FULL_NEAR_RANGE = 20.f;
F32 sNearMetres = FULL_NEAR_RANGE;

void updateNearRange(const asr::MemoryBudget& memory)
{
    constexpr F32 STEP_SECONDS = 1.f;
    static F32 steppedAt = 0.f;
    if (gFrameTimeSeconds - steppedAt < STEP_SECONDS) return;
    steppedAt = gFrameTimeSeconds;
    const U64 used = memory.used(asr::MemoryCategory::Textures);
    const U64 limit = memory.textureLimit();
    if (used > limit)
    {
        sNearMetres = llmax(sNearMetres * 0.8f, NEAREST_RANGE);
    }
    else if (used < limit / 10 * 9)
    {
        sNearMetres = llmin(sNearMetres * 1.1f, FULL_NEAR_RANGE);
    }
}

// The avatar menu's "Always render fully", as Firestorm saves it: LLVOAvatar keeps its copy protected.
bool alwaysRenderedFully(LLVOAvatar* avatar)
{
    return FSAvatarRenderPersistence::getInstance()->getAvatarRenderSettings(avatar->getID()) == LLVOAvatar::AV_ALWAYS_RENDER;
}

std::map<ShapeKey, std::unique_ptr<Shape>> sShapes;
std::unordered_map<const void*, TrackedTexture> sTextures; // by LLImageGL, which is what reports a change

// Texture feedback, from the GPU's slots back to textures. Slot 0 is the white texture, and
// many share it, so it has no owner.
std::vector<TrackedTexture*> sSlotOwners;
asr::TextureFeedback sFeedback;
F32 sLastFeedback = -1000.f; // when feedback last came, as gFrameTimeSeconds
U32 sFeedbackWindows = 0;    // temporary: since the last log
U32 sFeedbackSized = 0;      // temporary: textures sized by it, and judged hidden, since the last log
U32 sFeedbackHidden = 0;

// What the GPU saw since the last loop goes to the textures that hold those slots now.
void takeTextureFeedback()
{
    asr::Device* device = ASRViewer::device();
    if (!device || !device->takeTextureFeedback(sFeedback) || sSlotOwners.empty())
    {
        return;
    }
    sLastFeedback = gFrameTimeSeconds;
    sFeedbackWindows += sFeedback.windows;
    for (size_t i = 0; i < sFeedback.slots.size(); ++i)
    {
        TrackedTexture* owner = sSlotOwners[sFeedback.slots[i]];
        if (owner && owner->slotSince <= sFeedback.since)
        {
            // Each batch samples other pixels, so its level for one texture swings, and a
            // texture following it would shrink and sharpen again and again. A larger need
            // counts at once; a smaller one only once the larger has gone unmet for a while.
            constexpr F32 LEVEL_HOLD = 4.f; // seconds
            const U8 level = sFeedback.levels[i];
            owner->latestGpuLevel = level;
            if (level >= owner->gpuLevel || gFrameTimeSeconds - owner->gpuLevelAt > LEVEL_HOLD)
            {
                owner->gpuLevel = level;
                owner->gpuLevelAt = gFrameTimeSeconds;
            }
            owner->gpuSeenAt = gFrameTimeSeconds;
        }
    }
}

void ownSlot(TrackedTexture& texture, U32 slot)
{
    if (sSlotOwners.empty())
    {
        sSlotOwners.assign(asr::MAX_TEXTURES, nullptr);
    }
    if (texture.slot != 0 && sSlotOwners[texture.slot] == &texture)
    {
        sSlotOwners[texture.slot] = nullptr;
    }
    // Keep the demand window across mip replacements (the texture-cycling fix), but
    // after eviction no old feedback may suppress the first image of a reload.
    if (slot == 0)
    {
        texture.gpuSeenAt = -1000.f;
        texture.gpuLevelAt = -1000.f;
        texture.gpuLevel = 0;
    }
    texture.slot = slot;
    texture.slotSince = asr::TextureFeedback::Clock::now();
    if (slot != 0)
    {
        sSlotOwners[slot] = &texture;
    }
}
std::unordered_map<MaterialKey, SharedMaterial, BytesHash<MaterialKey>> sMaterials;
std::unordered_map<LLDrawable*, WorldObject> sObjects;
std::unordered_map<StackKey, Stack, BytesHash<StackKey>> sStacks;
std::unordered_map<LLDrawable*, const char*> sLeftOut; // what is not drawn yet, and why
std::unordered_set<LLDrawable*> sChanged;
std::unordered_set<LLDrawable*> sMoved;

// Render space is agent space plus this. A region crossing moves agent space under every
// object, but not render space: this takes up the difference, so nothing on the GPU changes.
// Rewriting every transform takes several frames of uploads, and the camera would be ahead
// of the world for those frames, which shows as a flash.
LLVector3 sRenderOrigin;

// How far the camera has moved, added up, and the objects whose LOD is next to change.
std::priority_queue<LodCheck> sLodChecks;
F64 sCameraTravel = 0.0;
LLVector3 sLastEye;
F32 sLastLodScale = 0.f;

std::vector<SweepEntry> sSweep; // every object, walked a share per frame: see sweepObjects()
size_t sSweepNext = 0;
std::vector<TrackedTexture*> sSharper; // only within one sweepLods(), so never stale
std::vector<const void*> sChangedImages;
std::chrono::steady_clock::time_point sLogged;

// Test tool: ASRendererLocalShift lifts the last clicked object in this window only, to
// show what sits behind it. A raw pointer, cleared in objectRemoved().
LLDrawable* sShifted = nullptr;
F32 sShift = 0.f;
U32 sLostTextures = 0; // temporary: textures that lost their image with faces on them
U32 sReleasedTextures = 0; // temporary: images given up by releaseTexture()

// How long a texture may stay out of view before it gives up its image: see updateUnseenLimit().
F32 sUnseenLimit = F32_MAX;

// Workers: they touch only their own volumes and the scene queue.
std::vector<std::thread> sWorkers;
std::mutex sJobMutex;
std::condition_variable sJobReady;
std::deque<ShapeJob> sJobs;
std::vector<ShapeJob> sFinished;
bool sStopping = false;

bool hasGeometry(const LLVolumeFace& face)
{
    // LLVolume represents an absent mesh face by one vertex and three zero indices.
    return face.mNumVertices >= 3 && face.mNumIndices >= 3;
}

// Adds one LOD to the fine end of a face's chain. Its box must already be set.
void addFaceLod(ShapeFace& face, U32 lod, LLVolumeFace& volumeFace)
{
    volumeFace.createTangents();

    std::vector<asr::SceneVertex> vertices(volumeFace.mNumVertices);
    for (S32 i = 0; i < volumeFace.mNumVertices; ++i)
    {
        asr::SceneVertex& vertex = vertices[i];
        memcpy(vertex.position, volumeFace.mPositions[i].getF32ptr(), sizeof(vertex.position));
        memcpy(vertex.normal, volumeFace.mNormals[i].getF32ptr(), sizeof(vertex.normal));
        memcpy(vertex.tangent, volumeFace.mTangents[i].getF32ptr(), sizeof(vertex.tangent));
        memcpy(vertex.uv, volumeFace.mTexCoords[i].mV, sizeof(vertex.uv));
    }

    // Each weight is a joint index plus its share, as LLSkinningUtil::getPerVertexSkinMatrix()
    // reads them: shares normalised, and a vertex with none goes wholly to its first joint.
    std::vector<asr::SkinWeights> weights;
    if (volumeFace.mWeights)
    {
        weights.resize(volumeFace.mNumVertices);
        for (S32 i = 0; i < volumeFace.mNumVertices; ++i)
        {
            const F32* packed = volumeFace.mWeights[i].getF32ptr();
            F32 shares[4];
            for (U32 k = 0; k < 4; ++k)
            {
                const F32 whole = floorf(packed[k]);
                weights[i].joints[k] = U8(llclamp(S32(whole), 0, S32(LL_MAX_JOINTS_PER_MESH_OBJECT) - 1));
                shares[k] = packed[k] - whole;
            }
            asr::packSkinAmounts(shares, weights[i].weights);
            for (U32 k = 0; k < 4; ++k)
            {
                if (weights[i].weights[k] != 0)
                {
                    face.skinJoints[lod] = llmax(face.skinJoints[lod], U32(weights[i].joints[k]) + 1);
                }
            }
        }
    }
    face.finest = sScene->addGeometry(asr::buildGeometry(vertices.data(), U32(vertices.size()), volumeFace.mIndices, U32(volumeFace.mNumIndices), &face.box,
                                                         weights.empty() ? nullptr : weights.data()),
                                      face.finest, COARSER_AT[lod]);
    face.geometries[lod] = face.finest;
    for (U32 k = 0; k < 3; ++k) // the last LOD built is the finest
    {
        face.extents[0][k] = volumeFace.mExtents[0][k];
        face.extents[1][k] = volumeFace.mExtents[1][k];
    }
}

void addFaceGeometry(ShapeFace& face, LLVolumeFace* lods[LODS])
{
    LLVector4a low(F32_MAX, F32_MAX, F32_MAX);
    LLVector4a high(-F32_MAX, -F32_MAX, -F32_MAX);
    for (U32 lod = 0; lod < LODS; ++lod)
    {
        if (lods[lod])
        {
            low.setMin(low, lods[lod]->mExtents[0]);
            high.setMax(high, lods[lod]->mExtents[1]);
        }
    }
    face.box.center[0] = (low[0] + high[0]) * 0.5f;
    face.box.center[1] = (low[1] + high[1]) * 0.5f;
    face.box.center[2] = (low[2] + high[2]) * 0.5f;
    face.box.scale = llmax((high[0] - low[0]) * 0.5f, (high[1] - low[1]) * 0.5f, (high[2] - low[2]) * 0.5f, 1e-6f);

    for (U32 lod = 0; lod < LODS; ++lod)
    {
        if (lods[lod])
        {
            addFaceLod(face, lod, *lods[lod]);
        }
    }
}

// Every LOD at once: a prim's are computed, and a sculpt's come from one map.
void buildShape(ShapeJob& job)
{
    LLPointer<LLVolume> volumes[LODS];
    const LLVolumeParams& params = job.shape->key.first;
    for (U32 lod = 0; lod < LODS; ++lod)
    {
        volumes[lod] = new LLVolume(params, LLVolumeLODGroup::getVolumeScaleFromDetail(lod));
        if (job.sculptMap.notNull())
        {
            LLImageDataSharedLock lock(job.sculptMap);
            volumes[lod]->sculpt(job.sculptMap->getWidth(), job.sculptMap->getHeight(), job.sculptMap->getComponents(), job.sculptMap->getData(),
                                 job.shape->key.second, job.sculptMissing);
        }
        else if (job.sculpted)
        {
            volumes[lod]->sculpt(0, 0, 0, nullptr, job.shape->key.second, job.sculptMissing);
        }
    }

    job.lodBias = volumes[LODS - 1]->mLODScaleBias;
    job.faces.resize(volumes[LODS - 1]->getNumVolumeFaces());
    for (S32 index = 0; index < S32(job.faces.size()); ++index)
    {
        LLVolumeFace* lods[LODS] = {};
        bool any = false;
        for (U32 lod = 0; lod < LODS; ++lod)
        {
            if (index < volumes[lod]->getNumVolumeFaces() && hasGeometry(volumes[lod]->getVolumeFace(index)))
            {
                lods[lod] = &volumes[lod]->getVolumeFace(index);
                any = true;
            }
        }
        if (any)
        {
            addFaceGeometry(job.faces[index], lods);
        }
    }
}

// All LODs of a face must share one box, and a mesh's finer LODs are not known yet. Mesh is
// made inside the object's unit cube, so that is the box. Every face then shares one grid
// of positions too, so faces cannot crack apart along a seam. snorm16() clamps anything
// outside it.
void addMeshLod(ShapeJob& job)
{
    job.lodBias = job.mesh->mLODScaleBias;
    job.faces.resize(llmax(job.faces.size(), size_t(job.mesh->getNumVolumeFaces())));
    for (S32 index = 0; index < job.mesh->getNumVolumeFaces(); ++index)
    {
        LLVolumeFace& volumeFace = job.mesh->getVolumeFace(index);
        if (hasGeometry(volumeFace))
        {
            ShapeFace& face = job.faces[index];
            face.box.scale = 0.5f;
            addFaceLod(face, job.lod, volumeFace);
        }
    }
}

void buildTerrainPatch(TerrainJob& job);

void workerLoop()
{
    while (true)
    {
        ShapeJob job;
        {
            std::unique_lock<std::mutex> lock(sJobMutex);
            sJobReady.wait(lock, []() { return sStopping || !sJobs.empty(); });
            if (sStopping)
            {
                return;
            }
            job = std::move(sJobs.front());
            sJobs.pop_front();
        }
        if (job.terrain)
        {
            buildTerrainPatch(*job.terrain);
        }
        else if (job.mesh.notNull())
        {
            addMeshLod(job);
        }
        else
        {
            buildShape(job);
        }
        std::lock_guard<std::mutex> lock(sJobMutex);
        sFinished.push_back(std::move(job));
    }
}

void startJob(ShapeJob&& job)
{
    job.shape->busy = true;
    std::lock_guard<std::mutex> lock(sJobMutex);
    sJobs.push_back(std::move(job));
    sJobReady.notify_one();
}

// A copy for the worker: the pixels of a texture can be replaced while it builds.
void startSculpt(Shape& shape)
{
    const LLVolumeParams& params = shape.key.first;
    ShapeJob job;
    job.shape = &shape;
    job.sculpted = true;
    LLViewerFetchedTexture* map = gTextureList.findImage(params.getSculptID(), TEX_LIST_STANDARD);
    if (map)
    {
        LLImageRaw* raw = map->getRawImage() ? map->getRawImage() : map->getSavedRawImage();
        job.sculptMissing = map->isMissingAsset();
        if (raw)
        {
            LLImageDataSharedLock lock(raw);
            const LLImageRaw* source = raw;
            job.sculptMap = new LLImageRaw(source->getData(), source->getWidth(), source->getHeight(), source->getComponents());
        }
    }
    startJob(std::move(job));
}

bool memoryPressure();
LLVOVolume* volumeOf(LLDrawable* drawable);

// Refresh from current users when the time-sliced demand sweep may have lagged. Both
// admission and eviction use this check, preventing lost late arrivals and reload churn.
bool meshLodNeeded(Shape& shape, U32 level)
{
    for (LLDrawable* user : shape.users)
    {
        LLVOVolume* volume = volumeOf(user);
        if (!volume || user->isDead()) continue;
        const S32 wanted = llclamp(volume->getLOD(), 0, S32(LODS) - 1);
        const U32 actual = U32(llmax(wanted, shape.actualLods[wanted]));
        if (actual >= level)
        {
            shape.residency.want(actual, gFrameTimeSeconds);
            return true;
        }
    }
    return false;
}

void startNextMeshLod(Shape& shape)
{
    if (shape.busy)
    {
        return;
    }
    for (; shape.nextLod < LODS; ++shape.nextLod)
    {
        const U32 lod = shape.nextLod;
        if (!shape.residency.holds(lod, gFrameTimeSeconds, memoryPressure()) && !meshLodNeeded(shape, lod))
        {
            // A late callback must not rebuild an unwanted/evicted fine LOD.
            for (U32 at = lod; at < LODS; ++at) shape.arrived[at] = nullptr;
            return;
        }
        if (shape.arrived[lod].notNull())
        {
            ShapeJob job;
            job.shape = &shape;
            job.faces = shape.faces;
            job.mesh = shape.arrived[lod];
            job.lod = lod;
            shape.arrived[lod] = nullptr;
            ++shape.nextLod;
            startJob(std::move(job));
            return;
        }

        // A LOD the mesh does not have only shows once its header is here.
        if (!shape.gone[lod] && gMeshRepo.getActualMeshLOD(shape.key.first, S32(lod)) == S32(lod))
        {
            return; // on its way, or nobody needs it yet
        }
    }
}

Shape* useShape(const ShapeKey& key, LLDrawable* drawable)
{
    auto found = sShapes.find(key);
    if (found == sShapes.end())
    {
        found = sShapes.emplace(key, std::make_unique<Shape>(key)).first;
        Shape& shape = *found->second;
        if ((key.first.getSculptType() & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_NONE)
        {
            ShapeJob job;
            job.shape = &shape;
            startJob(std::move(job));
        }
        else if (!isMeshKey(key))
        {
            startSculpt(shape);
        }
        // A mesh starts as its LODs come in: see ASRWorld::meshLoaded().
    }
    found->second->users.insert(drawable);
    return found->second.get();
}

void forgetShape(Shape* shape)
{
    for (const ShapeFace& face : shape->faces)
    {
        for (U32 geometry : face.geometries)
        {
            sScene->removeGeometry(geometry);
        }
    }
    sShapes.erase(shape->key);
}

// A shape a worker still holds goes when it comes back: see applyFinishedShapes().
void releaseShape(Shape* shape, LLDrawable* drawable)
{
    shape->users.erase(drawable);
    if (shape->users.empty() && !shape->busy)
    {
        forgetShape(shape);
    }
}

// Shorten unused LOD retention when the renderer reaches its working budget.
bool memoryPressure()
{
    asr::Device* device = ASRViewer::device();
    return device && device->memoryBudget().totalUsed() >= device->memoryBudget().budget();
}

// Asks for every LOD up to the one this object needs, coarsest included, all at once. The
// request stays alive for as long as one object wants it, and asking twice costs nothing.
// Once the header is known the real LODs are known too: a request for one the mesh does
// not have is dropped without a reply, so this runs again then.
void wantMeshLods(WorldObject& object, LLVOVolume* volume)
{
    Shape* latest = object.next ? object.next : object.shape;
    const LLVolumeParams& params = volume->getVolume()->getParams();
    if (!latest || !isMeshKey(latest->key) || params.getSculptID() != latest->key.first.getSculptID())
    {
        return; // it changes shape in applyObject(), and asks then
    }

    // Keep exactly the viewer's quality choice. Memory pressure changes retention time,
    // not the requested LOD, and demand is refreshed even when no new request is needed.
    const S32 wanted = llclamp(volume->getLOD(), 0, S32(LODS) - 1);
    latest->residency.want(U32(wanted), gFrameTimeSeconds);
    if (latest->actualLods[wanted] >= 0) latest->residency.want(U32(latest->actualLods[wanted]), gFrameTimeSeconds);
    if (wanted <= object.askedLod && object.askedWithHeader)
    {
        return; // the usual case, and it takes no lock
    }
    const bool header = gMeshRepo.hasHeader(params.getSculptID());
    if (wanted <= object.askedLod && header == object.askedWithHeader)
    {
        return;
    }
    object.askedLod = llmax(wanted, object.askedLod);
    object.askedWithHeader = header;

    // The object's own params, not the shape's key: the viewer's copy of this volume, which
    // picking reads, is looked up by them.
    const Shape& shape = *latest;
    for (S32 lod = 0; lod <= wanted; ++lod)
    {
        const S32 actual = gMeshRepo.getActualMeshLOD(params, lod);
        latest->actualLods[lod] = actual;
        if (actual >= 0) latest->residency.want(U32(actual), gFrameTimeSeconds);
        if (actual >= 0 && U32(actual) >= shape.nextLod && shape.arrived[actual].isNull())
        {
            gMeshRepo.loadMesh(volume, params, actual);
        }
    }
}

LLImageGL* imageOf(LLViewerTexture* image)
{
    if (!image->getGLTexture())
    {
        image->generateGLTexture();
    }
    return image->getGLTexture();
}

TrackedTexture* trackTexture(LLViewerTexture* image)
{
    auto found = sTextures.find(imageOf(image));
    if (found == sTextures.end())
    {
        found = sTextures.emplace(imageOf(image), TrackedTexture()).first;
        found->second.image = image;
        ownSlot(found->second, ASR::textureSlot(image->getTexName()));
        found->second.components = image->getComponents();
    }
    return &found->second;
}

TrackedTexture* useTexture(LLViewerTexture* image, LLDrawable* drawable, bool eye)
{
    TrackedTexture* texture = trackTexture(image);
    auto& uses = texture->users[drawable];
    ++uses.faces;
    uses.eyes += eye;
    return texture;
}

void forgetUnusedTexture(TrackedTexture* texture)
{
    if (texture->users.empty() && texture->terrainUsers == 0)
    {
        ownSlot(*texture, 0);
        sTextures.erase(imageOf(texture->image.get()));
    }
}

void releaseTexture(TrackedTexture* texture, LLDrawable* drawable, bool eye)
{
    auto user = texture->users.find(drawable);
    if (user != texture->users.end())
    {
        user->second.eyes -= eye;
        if (--user->second.faces == 0) texture->users.erase(user);
    }
    forgetUnusedTexture(texture);
}

MaterialKey materialKey(const asr::MaterialDesc& desc, const LLViewerTexture* image)
{
    MaterialKey key;
    F32* value = key.values;
    const auto add = [&value](const F32* from, size_t count)
    {
        memcpy(value, from, count * sizeof(F32));
        value += count;
    };
    add(desc.color, 4);
    add(desc.uvRow0, 3);
    add(desc.uvRow1, 3);
    add(&desc.alphaCutoff, 1);
    add(desc.planarScale, 3);
    add(desc.planarOffset, 3);
    key.planar = desc.planar ? 1 : 0;
    key.image = image;
    key.anim = desc.anim;
    return key;
}

// Where the viewer's default image is, which is what a face shows until its own texture
// has an image, as in OpenGL. Slot 0 is white, and left for faces with no texture at all.
U32 sDefaultSlot = 0;

U32 defaultImageSlot()
{
    LLViewerFetchedTexture* image = LLViewerFetchedTexture::sDefaultImagep.get();
    return image ? ASR::textureSlot(image->getTexName()) : 0;
}

U32 textureBits(const TrackedTexture* texture)
{
    const U32 slot = !texture ? 0 : texture->slot != 0 ? texture->slot : sDefaultSlot;
    return asr::uiTextureBits(slot, asr::UISampler::LinearWrap);
}

void writeMaterials(const TrackedTexture& texture)
{
    for (SharedMaterial* material : texture.materials)
    {
        material->desc.texture = textureBits(&texture);
        sScene->setMaterial(material->id, material->desc);
    }
}

SharedMaterial* useMaterial(const asr::MaterialDesc& desc, LLViewerTexture* image, LLDrawable* drawable, bool eye = false)
{
    TrackedTexture* texture = image ? useTexture(image, drawable, eye) : nullptr;
    const MaterialKey key = materialKey(desc, image);
    auto found = sMaterials.find(key);
    if (found == sMaterials.end())
    {
        found = sMaterials.emplace(key, SharedMaterial()).first;
        SharedMaterial& material = found->second;
        material.desc = desc;
        material.desc.texture = textureBits(texture);
        material.texture = texture;
        material.id = sScene->addMaterial(material.desc);
        if (texture)
        {
            texture->materials.push_back(&material);
        }
    }
    ++found->second.users;
    return &found->second;
}

void releaseMaterial(SharedMaterial* material, LLDrawable* drawable, bool eye = false)
{
    TrackedTexture* texture = material->texture;
    if (--material->users == 0)
    {
        sScene->removeMaterial(material->id);
        if (texture)
        {
            auto& materials = texture->materials;
            materials.erase(std::find(materials.begin(), materials.end(), material));
        }
        sMaterials.erase(materialKey(material->desc, texture ? texture->image.get() : nullptr));
    }
    if (texture)
    {
        releaseTexture(texture, drawable, eye);
    }
}

void clearFace(ObjectFace& face, LLDrawable* drawable)
{
    if (face.instance != NO_INSTANCE)
    {
        sScene->removeInstance(face.instance);
    }
    if (face.material)
    {
        releaseMaterial(face.material, drawable);
    }
    face = ObjectFace();
}

void leaveStack(LLDrawable* drawable, WorldObject& object);

void clearObject(LLDrawable* drawable, WorldObject& object)
{
    for (ObjectFace& face : object.faces)
    {
        clearFace(face, drawable);
    }
    object.faces.clear();
    leaveStack(drawable, object);
    if (object.shape)
    {
        releaseShape(object.shape, drawable);
        object.shape = nullptr;
    }
}

// Asks for a LOD check at this much camera travel, unless one is due sooner. An older check
// stays in the queue, and is dropped when it comes up, because its due no longer matches.
void scheduleLod(LLDrawable* drawable, WorldObject& object, F64 due)
{
    if (due < object.lodDue)
    {
        object.lodDue = due;
        sLodChecks.push({ due, drawable });
    }
}

void releaseBinding(LLDrawable* drawable, WorldObject& object);
std::unordered_set<LLDrawable*> sAwaitingAvatar; // worn before its avatar was built: tried again each frame

// Removes an object for good. sweepSlot stays true as entries swap places.
void dropObject(LLDrawable* drawable)
{
    auto found = sObjects.find(drawable);
    if (found == sObjects.end())
    {
        return;
    }

    WorldObject& object = found->second;
    clearObject(drawable, object);
    releaseBinding(drawable, object);
    sAwaitingAvatar.erase(drawable);
    if (object.next)
    {
        releaseShape(object.next, drawable);
    }
    sSweep[object.sweepSlot] = sSweep.back();
    sSweep[object.sweepSlot].object->sweepSlot = object.sweepSlot;
    sSweep.pop_back();
    sObjects.erase(found);
}

} // namespace

// A face's texture animation, from the frame counter LLViewerTextureAnim keeps to itself.
struct ASRTextureAnimClock
{
    static void fill(asr::TextureAnimDesc& anim, LLVOVolume* volume, S32 face, F32 rotation, F32 scaleS, F32 scaleT, F32 offsetS, F32 offsetT);
};

void ASRTextureAnimClock::fill(asr::TextureAnimDesc& anim, LLVOVolume* volume, S32 face, F32 rotation, F32 scaleS, F32 scaleT, F32 offsetS, F32 offsetT)
{
    anim = asr::TextureAnimDesc();
    const LLViewerTextureAnim* animation = volume->mTextureAnimp;
    if (!animation || !(animation->mMode & LLTextureAnim::ON) ||
        (animation->mFace >= 0 && animation->mFace < volume->getNumTEs() && animation->mFace != face))
    {
        return;
    }

    // The steady clock at this frame's start, which LLFrameTimer counts from: the same for
    // every face of the frame, so their materials still share.
    static U32 sFrame = 0;
    static S64 sAt = 0;
    if (sFrame != LLFrameTimer::getFrameCount() || sAt == 0)
    {
        sFrame = LLFrameTimer::getFrameCount();
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        sAt = std::chrono::duration_cast<std::chrono::microseconds>(now).count() - S64(LLFrameTimer::getCurrentFrameTime() * 1e6f);
    }

    // As animateTextures(): smooth, the timer restarts at each call and the count carries on.
    const F32 elapsed = animation->mTimer.getElapsedTimeF32() * animation->mRate;
    anim.at = sAt;
    anim.mode = animation->mMode;
    anim.counter = (animation->mMode & LLTextureAnim::SMOOTH) ? F32(animation->mLastTime) + elapsed : elapsed;
    anim.rate = animation->mRate;
    anim.sizeX = animation->mSizeX;
    anim.sizeY = animation->mSizeY;
    anim.start = animation->mStart;
    anim.length = animation->mLength;
    anim.rotation = rotation;
    anim.scaleS = scaleS;
    anim.scaleT = scaleT;
    anim.offsetS = offsetS;
    anim.offsetT = offsetT;
}

namespace
{

void setUVTransform(asr::MaterialDesc& material, F32 c, F32 s, F32 scaleS, F32 scaleT, F32 offsetS, F32 offsetT)
{
    asr::uvTransformRows(c, s, scaleS, scaleT, offsetS, offsetT, material.uvRow0, material.uvRow1);
}


// The material of a face. Returns false when the face shows nothing at all.
// Development only, while a GPU fault with real avatars is found: ASRendererAvatarSkip.
enum AvatarSkip : U32 { SKIP_RIGGED = 1, SKIP_ATTACHED = 2, SKIP_BLENDING = 4 };

bool skipping(U32 part)
{
    static LLCachedControl<U32> skip(gSavedSettings, "ASRendererAvatarSkip", 0);
    return (skip() & part) != 0;
}

// Drawn by Mac OIT, or as a cut-out where it is more than half opaque while skipping it.
void setBlended(U32& flags, asr::MaterialDesc& material)
{
    if (skipping(SKIP_BLENDING))
    {
        flags |= asr::INSTANCE_MASKED;
        material.alphaCutoff = 0.5f;
        return;
    }
    flags |= asr::INSTANCE_BLENDED;
}

bool fillMaterial(asr::MaterialDesc& material, U32& flags, LLViewerTexture*& image, LLVOVolume* volume, S32 index, const asr::GeometryBox& box)
{
    const LLTextureEntry* te = volume->getTE(index);
    if (!te)
    {
        return false;
    }

    flags = 0;
    image = nullptr;
    LLFetchedGLTFMaterial* gltf = static_cast<LLFetchedGLTFMaterial*>(te->getGLTFRenderMaterial());
    if (gltf)
    {
        image = gltf->mBaseColorTexture.get();
        memcpy(material.color, gltf->mBaseColor.mV, sizeof(material.color));
        if (gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_OPAQUE)
        {
            // glTF's opaque mode ignores alpha, and OpenGL draws such a face whatever its
            // base colour alpha: zero must not drop it below.
            material.color[3] = 1.f;
        }
        if (gltf->mDoubleSided)
        {
            flags |= asr::INSTANCE_DOUBLE_SIDED;
        }
        if (gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK)
        {
            flags |= asr::INSTANCE_MASKED;
            material.alphaCutoff = gltf->mAlphaCutoff;
        }
        else if (gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND)
        {
            setBlended(flags, material);
        }

        // Base colour UVs flip into glTF's space, transform there, and flip back.
        const LLGLTFMaterial::TextureTransform& transform = gltf->mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
        const F32 c = cosf(transform.mRotation);
        const F32 s = sinf(transform.mRotation);
        const F32 scaleS = transform.mScale.mV[VX];
        const F32 scaleT = transform.mScale.mV[VY];
        material.uvRow0[0] = c * scaleS;
        material.uvRow0[1] = -s * scaleT;
        material.uvRow0[2] = s * scaleT + transform.mOffset.mV[VX];
        material.uvRow1[0] = s * scaleS;
        material.uvRow1[1] = c * scaleT;
        material.uvRow1[2] = 1.f - c * scaleT - transform.mOffset.mV[VY];
    }
    else
    {
        image = volume->getTEImage(index);
        memcpy(material.color, te->getColor().mV, sizeof(material.color));

        // A HUD's faces have no legacy material, as OpenGL's rebuildGeom() ignores it for
        // HUD groups: any alpha in texture or colour blends, never cut or dropped.
        const LLMaterialPtr params = volume->isHUDAttachment() ? LLMaterialPtr() : te->getMaterialParams();
        const U8 alphaMode = params.notNull() ? params->getDiffuseAlphaMode() : U8(LLMaterial::DIFFUSE_ALPHA_MODE_BLEND);
        const bool textureAlpha = image && image->getComponents() == 4;
        if (alphaMode == LLMaterial::DIFFUSE_ALPHA_MODE_MASK)
        {
            flags |= asr::INSTANCE_MASKED;
            material.alphaCutoff = F32(params->getAlphaMaskCutoff()) / 255.f;
        }
        else if (material.color[3] < 1.f || (alphaMode == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND && textureAlpha))
        {
            setBlended(flags, material);
        }

        F32 scaleS = 1.f;
        F32 scaleT = 1.f;
        F32 offsetS = 0.f;
        F32 offsetT = 0.f;
        te->getScale(&scaleS, &scaleT);
        te->getOffset(&offsetS, &offsetT);
        setUVTransform(material, cosf(te->getRotation()), sinf(te->getRotation()), scaleS, scaleT, offsetS, offsetT);
        ASRTextureAnimClock::fill(material.anim, volume, index, te->getRotation(), scaleS, scaleT, offsetS, offsetT);

        if (te->getTexGen() == LLTextureEntry::TEX_GEN_PLANAR)
        {
            const LLVector3& scale = volume->getScale();
            material.planar = true;
            for (U32 axis = 0; axis < 3; ++axis)
            {
                material.planarScale[axis] = box.scale * scale.mV[axis];
                material.planarOffset[axis] = box.center[axis] * scale.mV[axis];
            }
        }
    }

    // A surface that is fully transparent is there to be clicked, not seen.
    if (material.color[3] <= 0.f)
    {
        return false;
    }

    return true;
}

// The matrix GL draws this object with: from its volume to its partition, then to agent space.
void fillTransform(asr::InstanceDesc& desc, LLDrawable* drawable, LLVOVolume* volume, const Shape& shape)
{
    LLMatrix4 matrix = volume->getRelativeXform();
    if (drawable->isState(LLDrawable::ANIMATED_CHILD))
    {
        // A child moving against its parent, such as a part spun by llTargetOmega. Its world
        // matrix already holds its place and rotation, so only its scale may be added: the
        // relative matrix holds them too, and using it would apply them twice and swing the
        // part around its link position. rebuildGeom() resets it for this, and does not run here.
        const LLVector3& scale = drawable->getScale();
        matrix.initRows(LLVector4(scale.mV[VX], 0.f, 0.f, 0.f), LLVector4(0.f, scale.mV[VY], 0.f, 0.f), LLVector4(0.f, 0.f, scale.mV[VZ], 0.f), LLVector4(0.f, 0.f, 0.f, 1.f));
        matrix *= drawable->getWorldMatrix();
    }
    else if (drawable->isActive())
    {
        matrix *= drawable->getRenderMatrix();
    }
    else if (drawable->getRegion())
    {
        matrix *= drawable->getRegion()->mRenderMatrix;
    }

    for (U32 row = 0; row < 3; ++row)
    {
        for (U32 column = 0; column < 4; ++column)
        {
            desc.objectToWorld[row * 4 + column] = matrix.mMatrix[column][row]; // LLMatrix4 holds rows of a row-vector matrix
        }
        desc.lodCenter[row] = matrix.mMatrix[3][row];
        if (!volume->isHUDAttachment()) // HUD space has no world origin
        {
            desc.objectToWorld[row * 4 + 3] += sRenderOrigin.mV[row];
            desc.lodCenter[row] += sRenderOrigin.mV[row];
        }
    }
    if (drawable == sShifted)
    {
        desc.objectToWorld[11] += sShift;
        desc.lodCenter[2] += sShift;
    }
    desc.lodSize = shape.lodBias.scaledVec(volume->getScale()).length();
}

// Avatars. Each has one skeleton on the GPU, posed from the viewer's joints whenever they
// move, which the GPU carries on to the frame's time. A rigged mesh is skinned to it there,
// and an attachment follows its attachment point there, so an avatar that moves writes its
// joints and nothing else.
std::unordered_map<LLVOAvatar*, AvatarSkeleton> sSkeletons;
std::map<SkinKey, SharedSkin> sSkins;
std::vector<LLVOAvatar*> sBodyAvatars; // rebuilt from live characters each update; never retained as ownership

// Three rows of a row-vector matrix, as the GPU takes them.
void rowsOf(const LLMatrix4& matrix, F32* rows)
{
    for (U32 row = 0; row < 3; ++row)
    {
        for (U32 column = 0; column < 4; ++column)
        {
            rows[row * 4 + column] = matrix.mMatrix[column][row];
        }
    }
}

// Every joint LLVOAvatar::getJoint() numbers: bones, then collision volumes, then attachment
// points, which come last.
U32 skeletonJoints(LLVOAvatar* avatar)
{
    U32 joints = 0;
    for (const auto& point : avatar->mAttachmentPoints)
    {
        joints = llmax(joints, U32(llmax(point.second->getJointNum(), 0)) + 1);
    }
    return joints;
}

// Every this many frames each skeleton is read even if the viewer did not animate it, in
// case its joints moved some other way.
constexpr U32 POSE_SWEEP_FRAMES = 64;

// Sends the avatar's pose if it changed. Its joints are only read when they can have moved:
// the viewer animated it (ASRWorld::avatarAnimated()), it animates every frame, the render
// origin moved, or the sweep is due. Returns whether they were read.
bool poseSkeleton(LLVOAvatar* avatar, AvatarSkeleton& skeleton)
{
    const bool read = skeleton.animated || skeleton.animationPeriod <= 1 || skeleton.rows.empty() || skeleton.origin != sRenderOrigin ||
                      (LLDrawable::getCurrentFrame() + skeleton.id) % POSE_SWEEP_FRAMES == 0;
    skeleton.animated = false;
    if (!read)
    {
        // Far avatars animate every few frames (setAnimationPeriods()). When a whole period
        // brings no new pose, the last is sent once more, and the GPU stops moving it.
        ++skeleton.unchangedFrames;
        if (!skeleton.settled && skeleton.unchangedFrames > U32(skeleton.animationPeriod))
        {
            sScene->poseSkeleton(skeleton.id, skeleton.rows.data(), skeleton.joints, std::chrono::steady_clock::now());
            skeleton.settled = true;
        }
        return false;
    }
    skeleton.origin = sRenderOrigin;

    // A joint the avatar does not have stands at its root, and so do HUD attachment points,
    // which are in screen space: anywhere far off would stretch the box the GPU packs skinned
    // vertices in, and its 16-bit positions would snap to a coarse grid.
    static std::vector<F32> rows; // reused: the main thread only
    rows.resize(size_t(skeleton.joints) * 12);
    LLMatrix4 root = avatar->getRootJoint() ? avatar->getRootJoint()->getWorldMatrix() : LLMatrix4();
    for (U32 joint = 0; joint < skeleton.joints; ++joint)
    {
        LLJoint* found = avatar->getJoint(S32(joint));
        LLViewerJointAttachment* point = dynamic_cast<LLViewerJointAttachment*>(found);
        LLMatrix4 matrix = found && !(point && point->getIsHUDAttachment()) ? found->getWorldMatrix() : root;
        matrix.mMatrix[3][0] += sRenderOrigin.mV[0];
        matrix.mMatrix[3][1] += sRenderOrigin.mV[1];
        matrix.mMatrix[3][2] += sRenderOrigin.mV[2];
        rowsOf(matrix, &rows[size_t(joint) * 12]);
    }

    // Only a new pose is sent, so the GPU moves far avatars smoothly between poses.
    const bool changed = rows != skeleton.rows;
    skeleton.unchangedFrames = changed ? 0 : skeleton.unchangedFrames + 1;
    if (changed || (!skeleton.settled && skeleton.unchangedFrames > U32(skeleton.animationPeriod)))
    {
        // Interpolated: the gap the GPU blends over is at most the period, with room for
        // uneven frames, so the first pose after a rest does not take seconds.
        const F32 gap = skeleton.interpolate ? F32(skeleton.animationPeriod) * llmax(gFrameIntervalSeconds.value(), 0.001f) * 1.5f : 0.f;
        sScene->poseSkeleton(skeleton.id, rows.data(), skeleton.joints, std::chrono::steady_clock::now(),
                             std::chrono::microseconds(S64(gap * 1e6f)));
        skeleton.settled = !changed;
        skeleton.rows.swap(rows);
    }
    return true;
}

AvatarSkeleton& skeletonOf(LLVOAvatar* avatar)
{
    AvatarSkeleton& skeleton = sSkeletons[avatar];
    if (skeleton.id == 0)
    {
        skeleton.joints = skeletonJoints(avatar);
        skeleton.id = sScene->addSkeleton(skeleton.joints);
        poseSkeleton(avatar, skeleton); // posed before anything is drawn on it
        // Identify the extra skeleton when diagnosing transparency faults around arriving avatars or animesh.
        LL_INFOS("ASRenderer") << "created skeleton " << skeleton.id << " for "
                                << (avatar->isControlAvatar() ? "animesh " : avatar->isSelf() ? "self avatar " : "avatar ")
                                << avatar->getID() << ", " << skeleton.joints << " joints" << LL_ENDL;
    }
    return skeleton;
}

AvatarSkeleton* useSkeleton(LLVOAvatar* avatar, LLDrawable* drawable)
{
    AvatarSkeleton& skeleton = skeletonOf(avatar);
    skeleton.users.insert(drawable);
    return &skeleton;
}

SharedSkin* useSkin(const LLMeshSkinInfo* info, LLVOAvatar* avatar, SkinKey& key)
{
    key = SkinKey(info->mMeshID, info->mHash);
    SharedSkin& skin = sSkins[key];
    if (skin.id == 0)
    {
        // Every avatar numbers its joints alike, so one lookup serves them all.
        LLSkinningUtil::initJointNums(const_cast<LLMeshSkinInfo*>(info), avatar);
        const U32 joints = llmin(LLSkinningUtil::getMeshJointCount(info), U32(info->mBindPoseMatrix.size()), U32(info->mJointNums.size()));
        asr::SkinDesc desc;
        desc.matrices.resize(size_t(joints) * 12);
        desc.joints.resize(joints);
        for (U32 joint = 0; joint < joints; ++joint)
        {
            // Bind shape then inverse bind, as LLFace bakes the first into the vertices and
            // initSkinningMatrixPalette() puts the second before the joint.
            rowsOf(LLMatrix4(info->mBindPoseMatrix[joint].getF32ptr()), &desc.matrices[size_t(joint) * 12]);
            desc.joints[joint] = U16(llmax(info->mJointNums[joint], 0));
        }
        skin.joints = joints;
        skin.id = joints ? sScene->addSkin(std::move(desc)) : 0;
    }
    ++skin.users;
    return &skin;
}

void releaseBinding(LLDrawable* drawable, WorldObject& object)
{
    if (!object.bound)
    {
        return;
    }
    const auto skeleton = sSkeletons.find(object.bound->avatar);
    if (skeleton != sSkeletons.end())
    {
        skeleton->second.users.erase(drawable); // removed in updateAvatars(), if that was the last
    }
    if (object.bound->skin && --object.bound->skin->users == 0)
    {
        if (object.bound->skin->id != 0)
        {
            sScene->removeSkin(object.bound->skin->id);
        }
        sSkins.erase(object.bound->skinKey);
    }
    object.bound.reset();
}

// The avatar it is drawn on, if any: a rigged mesh on the one it is rigged to, whether worn
// or animated, and anything else worn on its wearer. The other prims of an animated object
// stand in the world.
LLVOAvatar* boundAvatar(LLVOVolume* volume, bool& rigged)
{
    if (volume->isHUDAttachment())
    {
        rigged = false;
        return nullptr; // in HUD space, placed by its own matrix: see fillTransform()
    }
    LLVOAvatar* avatar = volume->getAvatar();
    rigged = volume->isRiggedMesh() && avatar && volume->getSkinInfo();
    return rigged ? avatar : volume->getAvatarAncestor();
}

// An attached prim's matrix in its attachment point's space. The root's place is kept
// against that point, and the other prims' against the root.
void attachedMatrix(LLDrawable* drawable, LLVOVolume* volume, LLViewerJointAttachment* point, F32* rows)
{
    LLMatrix4 matrix = volume->getRelativeXform();
    if (drawable->isState(LLDrawable::ANIMATED_CHILD))
    {
        // Spun against its root: as fillTransform(), from its world matrix.
        const LLVector3& scale = drawable->getScale();
        matrix.initRows(LLVector4(scale.mV[VX], 0.f, 0.f, 0.f), LLVector4(0.f, scale.mV[VY], 0.f, 0.f), LLVector4(0.f, 0.f, scale.mV[VZ], 0.f), LLVector4(0.f, 0.f, 0.f, 1.f));
        matrix *= drawable->getWorldMatrix();
        LLMatrix4 toPoint = point->getWorldMatrix();
        toPoint.invert();
        matrix *= toPoint;
    }
    else
    {
        LLDrawable* root = drawable->isRoot() ? drawable : drawable->getParent();
        matrix *= LLMatrix4(root->getRotation(), LLVector4(root->getPosition(), 1.f));
    }
    rowsOf(matrix, rows);
}

// Fills desc for an object drawn on an avatar. False when it is not drawn yet, or at all.
bool bindToAvatar(LLDrawable* drawable, LLVOVolume* volume, WorldObject& object, asr::InstanceDesc& desc)
{
    bool rigged = false;
    LLVOAvatar* avatar = boundAvatar(volume, rigged);
    const LLMeshSkinInfo* info = rigged ? volume->getSkinInfo() : nullptr;
    const SkinKey skinKey = info ? SkinKey(info->mMeshID, info->mHash) : SkinKey();
    if (object.bound && (object.bound->avatar != avatar || object.bound->skinKey != skinKey))
    {
        releaseBinding(drawable, object);
    }
    if (!avatar || avatar->isDead() || !avatar->isBuilt())
    {
        return false;
    }

    LLViewerJointAttachment* point = nullptr;
    if (!rigged)
    {
        point = avatar->getTargetAttachmentPoint(volume->getRootEdit());
        if (!point || point->getJointNum() < 0)
        {
            return false;
        }
    }
    if (!object.bound)
    {
        object.bound = std::make_unique<AvatarBinding>();
        object.bound->avatar = avatar;
        if (info)
        {
            object.bound->skin = useSkin(info, avatar, object.bound->skinKey);
        }
    }
    AvatarSkeleton* skeleton = useSkeleton(avatar, drawable);

    desc.skeleton = skeleton->id;
    if (rigged)
    {
        // GL's volume batching normally sets this flag. Native rendering does not run
        // that batching, but CPU LOD selection and ray picking still depend on it.
        // As there, the first time it is marked, the LOD is measured again: until then
        // calcLOD() measured the attachment's own small size, not its avatar's.
        const bool wasRigged = drawable->isState(LLDrawable::RIGGED);
        drawable->setState(LLDrawable::RIGGED);
        LLDrawable* root = drawable->getRoot();
        if (root && root != drawable) root->setState(LLDrawable::RIGGED_CHILD);
        if (!wasRigged)
        {
            volume->updateLOD();
        }
        // As LLVOVolume::calcLOD() measures a rigged mesh: by its avatar's animated extents.
        // Not by its own size: creators often leave a rigged mesh's coarser LODs empty or
        // broken, as the viewer only shows them with the whole avatar far away, and outfits
        // then lost parts at a few metres.
        desc.skin = object.bound->skin->id;
        const LLVector3* extents = avatar->getLastAnimExtents();
        desc.lodSize = llmax((extents[1] - extents[0]).magVec() * (avatar->isControlAvatar() ? 0.5f : 1.f), 0.1f);
        return true; // a skin with no joints draws nothing: see skinnable()
    }
    object.bound->joint = U32(point->getJointNum());
    desc.joint = object.bound->joint;
    attachedMatrix(drawable, volume, point, object.bound->local);
    memcpy(desc.objectToWorld, object.bound->local, sizeof(desc.objectToWorld));
    desc.lodCenter[0] = desc.objectToWorld[3];
    desc.lodCenter[1] = desc.objectToWorld[7];
    desc.lodCenter[2] = desc.objectToWorld[11];
    desc.lodSize = object.shape->lodBias.scaledVec(volume->getScale()).length();
    return true;
}

// A usable weighted LOD is enough; the GPU skips unsuitable neighbours in its chain.
bool skinnable(const ShapeFace& face, const WorldObject& object)
{
    const SharedSkin* skin = object.bound ? object.bound->skin : nullptr;
    if (!skin) return true;
    for (U32 lod = 0; skin->id != 0 && lod < LODS; ++lod)
    {
        if (face.skinJoints[lod] != 0 && face.skinJoints[lod] <= skin->joints) return true;
    }
    return false;
}

// Poses every skeleton something is drawn on, system bodies included, and lets go of those
// nothing is.
// The nearest ASRendererFullRateAvatars avatars in view, yours first, animate every frame;
// the others less often: by their distance to the camera, or rarely when out of view.
// Animating them is most of the main thread's time in crowds. The gaps are times, turned
// into frames at the current loop rate, so a slow main thread does not make poses older.
// Only new poses reach the GPU, which blends between them one gap late, so they move
// smoothly and never overshoot. One seated on something moving keeps a short period and
// is carried on instead: late, it would trail its seat.
// LLVOAvatar::computeNeedsUpdate() reads these through ASRWorld::animationPeriod().
void setAnimationPeriods()
{
    static LLCachedControl<U32> fullRate(gSavedSettings, "ASRendererFullRateAvatars", 16);
    static LLCachedControl<bool> park(gSavedSettings, "ASRendererParkAvatars", true);
    const asr::MemoryBudget& memory = ASRViewer::device()->memoryBudget();
    sAvatarResidency.update(memory.totalUsed(), memory.budget(), gFrameTimeSeconds);
    // Smoothed, so one slow frame does not reshuffle every period.
    static F32 loopSeconds = 1.f / 60.f;
    loopSeconds += (llclamp(F32(gFrameIntervalSeconds.value()), 0.001f, 0.5f) - loopSeconds) * 0.05f;

    struct Ranked
    {
        F32 distance;
        bool inView;
        bool onMovingSeat;
        AvatarSkeleton* skeleton;
        bool alwaysFull; // the local avatar, the camera's focus, and those set to always render fully
    };
    std::vector<Ranked> ranked;
    ranked.reserve(sSkeletons.size());
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLViewerObject* focus = gAgentCamera.getFocusObject().get();
    const LLViewerObject* focusAvatar = focus ? (focus->isAvatar() ? focus : focus->getAvatar()) : nullptr;
    const F32 aspect = camera->getAspect();
    const F32 halfCorner = atanf(tanf(camera->getView() * 0.5f) * sqrtf(1.f + aspect * aspect));
    for (auto& [avatar, skeleton] : sSkeletons)
    {
        bool inView = false;
        F32 distance = 0.f;
        if (avatar->mDrawable.notNull())
        {
            // From where the avatar is drawn this frame, seated ones included. Not from its
            // drawable's spatial extents and distance: the viewer's own render pipeline keeps
            // those up to date, and with this renderer drawing they can lag far behind, so an
            // avatar right in view counted as out of it.
            constexpr F32 LEAST_RADIUS = 1.5f; // metres: a standing avatar's half height and some
            const LLVector3 middle = avatar->getRenderPosition();
            const LLVector3* box = avatar->getLastAnimExtents();
            const F32 radius = llmax((box[1] - box[0]).length() * 0.5f, LEAST_RADIUS);
            const LLVector3 to = middle - camera->getOrigin();
            distance = to.length();
            inView = camera->sphereInFrustum(middle, radius) != 0;

            // Keep a margin for render-thread camera turns between viewer updates.
            const bool wanted = inView || avatar->isSelf() || !park || distance <= radius ||
                sAvatarResidency.nearView((to / distance) * camera->getAtAxis(), radius / distance, halfCorner);
            const bool resident = sAvatarResidency.keepResident(wanted, gFrameTimeSeconds, skeleton.nearViewAt);
            if (resident != skeleton.resident && skeleton.id != 0)
            {
                skeleton.resident = resident;
                sScene->setSkeletonResident(skeleton.id, resident);
            }
        }
        const LLViewerObject* seat = static_cast<const LLViewerObject*>(avatar->getParent());
        const bool moving = seat && (seat->getVelocity().lengthSquared() > 0.01f || seat->getAngularVelocity().lengthSquared() > 0.01f);
        const bool alwaysFull = avatar->isSelf() || avatar == focusAvatar || alwaysRenderedFully(avatar) ||
                                (inView && distance <= asr::AvatarDetailPolicy::NEAR_METRES);
        ranked.push_back({ avatar->isSelf() ? -1.f : distance, avatar->isSelf() || inView, moving, &skeleton, alwaysFull });
    }
    // In view first, then nearest; ties keep a stable order so places do not churn.
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b)
    {
        return a.inView != b.inView ? a.inView : a.distance < b.distance;
    });
    for (size_t rank = 0; rank < ranked.size(); ++rank)
    {
        const Ranked& entry = ranked[rank];
        F32 gap = 0.f; // seconds between poses
        if (entry.onMovingSeat && rank >= fullRate())
        {
            gap = 2.f * loopSeconds;
        }
        else if (rank >= fullRate())
        {
            gap = !entry.inView ? 1.f
                : entry.distance <= 4.f ? 0.12f
                : entry.distance <= 8.f ? 0.25f
                : entry.distance <= 16.f ? 0.5f
                : entry.distance <= 32.f ? 1.f
                : 2.f;
        }
        const S32 period = llclamp(S32(gap / loopSeconds + 0.5f), 1, 128);
        entry.skeleton->animationPeriod = period;
        entry.skeleton->interpolate = period > 2 && !entry.onMovingSeat;
    }

    // Crowd detail ranking, in the same order: in view first, then nearest. LLVOVolume::calcLOD()
    // reads the cap through ASRWorld::avatarLodCap(), and the LOD sweep applies a change.
    const U64 textures = memory.used(asr::MemoryCategory::Textures);
    const U64 others = memory.totalUsed() > textures ? memory.totalUsed() - textures : 0;
    updateNearRange(memory);
    const bool overcommitted = memory.totalUsed() > memory.budget() && sNearMetres <= NEAREST_RANGE;
    sAvatarDetail.update(others, memory.totalUsed(), memory.budget(), ranked.size(), gFrameTimeSeconds, overcommitted);
    size_t detailRank = 0;
    for (const Ranked& entry : ranked)
    {
        entry.skeleton->lodCap = entry.alwaysFull ? asr::AvatarDetailPolicy::FINEST : sAvatarDetail.lodCap(detailRank++);
    }
}

// An avatar showing only its system body while the simulator lists attachments gets them
// back: what the viewer holds is applied again (the renderer dropped it), and what it
// lacks is asked of the simulator again (the update never arrived). After 10 s, then
// doubling up to 2 minutes, so an avatar the simulator does not complete is not flooded.
void recoverBodyOnlyAvatars()
{
    struct Retry { F32 at; F32 wait; F32 since; };
    static std::unordered_map<LLVOAvatar*, Retry> retries;
    bool sendEverythingAround = false;

    // Whether the simulator is still sending the region: objects arriving after login or a
    // teleport. The default interest list sends what is in front of the camera first; 360
    // then would queue everything behind it ahead of the attachments in view.
    constexpr F32 QUIET_SAMPLE_SECONDS = 10.f;
    constexpr S32 QUIET_NEW_OBJECTS = 50;
    static F32 sampledAt = 0.f;
    static S32 sampledObjects = 0;
    static bool regionSent = false;
    if (gFrameTimeSeconds - sampledAt >= QUIET_SAMPLE_SECONDS)
    {
        const S32 objects = gObjectList.getNumObjects();
        regionSent = objects - sampledObjects < QUIET_NEW_OBJECTS;
        sampledObjects = objects;
        sampledAt = gFrameTimeSeconds;
    }
    for (auto entry = retries.begin(); entry != retries.end();)
    {
        const auto found = sSkeletons.find(entry->first);
        const bool bodyOnly = found != sSkeletons.end() && found->second.users.empty() && found->second.body;
        entry = bodyOnly ? std::next(entry) : retries.erase(entry);
    }
    for (auto& [avatar, skeleton] : sSkeletons)
    {
        if (!skeleton.users.empty() || !skeleton.body || avatar->isDead() || avatar->isControlAvatar() || avatar->mSimAttachments.empty()) continue;
        const auto [retry, added] = retries.emplace(avatar, Retry{ gFrameTimeSeconds, 10.f, gFrameTimeSeconds });
        if (added || gFrameTimeSeconds - retry->second.at < retry->second.wait) continue;
        retry->second.at = gFrameTimeSeconds;
        retry->second.wait = llmin(retry->second.wait * 2.f, 120.f);

        U32 reapplied = 0;
        U32 held = 0; // attachments on the viewer: getNumAttachments() is protected
        for (const auto& [point, attachment] : avatar->mAttachmentPoints)
        {
            for (const auto& attached : attachment->mAttachedObjects)
            {
                if (!attached || attached->isDead()) continue;
                ++held;
                if (attached->mDrawable.notNull()) { sChanged.insert(attached->mDrawable); ++reapplied; }
                for (LLViewerObject* child : attached->getChildren())
                {
                    if (child && child->mDrawable.notNull()) { sChanged.insert(child->mDrawable); ++reapplied; }
                }
            }
        }
        U32 unattached = 0;
        U32 missing = 0;
        for (const auto& [id, point] : avatar->mSimAttachments)
        {
            LLViewerObject* object = gObjectList.findObject(id);
            if (!object) ++missing;
            else if (!object->isAttachment()) { object->requestObjectUpdate(); ++unattached; }
        }
        // Asking for the avatar again does not bring them: the simulator's interest list held
        // them back. Area Search's 360 mode releases them, so recovery borrows it briefly, but
        // only for an avatar bare for a minute once the region is sent: at a crowded arrival,
        // 360 delayed the attachments in view by minutes.
        constexpr F32 BARE_BEFORE_360 = 60.f;
        sendEverythingAround |= missing != 0 && regionSent && gFrameTimeSeconds - retry->second.since >= BARE_BEFORE_360;
        LL_INFOS("ASRenderer") << "body only: " << avatar->getDebugName() << ", " << avatar->mSimAttachments.size() << " attachments by the simulator, "
                               << held << " on the viewer; " << reapplied << " objects applied again, " << unattached
                               << " unattached and " << missing << " missing asked again; rezzed status " << avatar->getRezzedStatus()
                               << ", fully loaded " << avatar->isFullyLoaded() << ", lod cap " << skeleton.lodCap << LL_ENDL;
    }

    // The 360 interest list for a few seconds, at most once a minute: the simulator then sends
    // everything within draw distance, missing attachments included. Left alone while Area
    // Search or anything else has chosen another mode.
    constexpr F32 SEND_ALL_SECONDS = 5.f;
    constexpr F32 SEND_ALL_EVERY = 60.f;
    static bool sendingAll = false;
    static F32 sendAllUntil = 0.f;
    static F32 sendAllNext = 0.f;
    if (sendEverythingAround && !sendingAll && gFrameTimeSeconds >= sendAllNext && !gAgent.getFSAreaSearchActive() &&
        gAgent.getInterestListMode() == IL_MODE_DEFAULT)
    {
        gAgent.changeInterestListMode(IL_MODE_360);
        sendingAll = true;
        sendAllUntil = gFrameTimeSeconds + SEND_ALL_SECONDS;
        sendAllNext = gFrameTimeSeconds + SEND_ALL_EVERY;
        LL_INFOS("ASRenderer") << "body only: interest list set to 360 for " << SEND_ALL_SECONDS << " s to bring missing attachments" << LL_ENDL;
    }
    else if (sendingAll && gFrameTimeSeconds >= sendAllUntil)
    {
        sendingAll = false;
        if (!gAgent.getFSAreaSearchActive() && gAgent.getInterestListMode() == IL_MODE_360)
        {
            gAgent.changeInterestListMode(IL_MODE_DEFAULT);
        }
    }
}

void updateAvatars()
{
    std::unordered_set<LLVOAvatar*> alive;
    sBodyAvatars.clear();
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
        alive.insert(avatar);
        if (ASRAvatarBody::wanted(avatar))
        {
            sBodyAvatars.push_back(avatar); // system bodies get a turn after mesh work
        }
    }
    for (auto entry = sSkeletons.begin(); entry != sSkeletons.end();)
    {
        AvatarSkeleton& skeleton = entry->second;
        const bool gone = alive.count(entry->first) == 0 || entry->first->isDead();
        if (gone)
        {
            // Its objects normally went first. Any left are dropped with it.
            std::vector<LLDrawable*> users(skeleton.users.begin(), skeleton.users.end());
            for (LLDrawable* drawable : users)
            {
                dropObject(drawable);
            }
            skeleton.users.clear();
        }
        if (skeleton.body && (gone || !ASRAvatarBody::wanted(entry->first)))
        {
            ASRAvatarBody::release(skeleton.body); // reads nothing of a gone avatar
        }
        if (skeleton.users.empty() && !skeleton.body && (gone || !ASRAvatarBody::wanted(entry->first)))
        {
            sScene->removeSkeleton(skeleton.id);
            entry = sSkeletons.erase(entry);
            continue;
        }
        // GPU culling replaces updateCull(), which normally refreshes these CPU bounds
        // and distances. Keep them current for rigged mesh LOD requests and picking.
        LLVOAvatar* avatar = entry->first;
        if (avatar->mDrawable.notNull())
        {
            LLVector4a low, high;
            avatar->updateSpatialExtents(low, high);
            avatar->mDrawable->setSpatialExtents(low, high);
            avatar->mDrawable->movePartition();
            // The distance part of LLDrawable::updateDistance(), without invoking
            // the system avatar's GL mesh/VBO LOD rebuild. From where the avatar is drawn this
            // frame: its position group follows the spatial extents, which only the viewer's own
            // render pipeline keeps up to date, and a stale far distance gives an avatar right
            // here the coarse LODs of everything it wears, often empty.
            const LLVector3 center = avatar->getRenderPosition();
            avatar->mDrawable->mDistanceWRTCamera = llmax(0.01f, ll_round((center - LLViewerCamera::getInstance()->getOrigin()).length(), 0.01f));

            // LOD checks follow camera travel, which only suits what stands still. A worn
            // object moves with its avatar, and a teleport or an avatar walking up changes
            // its distance with no travel, so these are checked when the avatar's own
            // distance changes by a tenth. LOD limits are distance ratios, hence relative.
            const F32 distance = avatar->mDrawable->mDistanceWRTCamera;
            if (skeleton.lodDistance < 0.f || fabsf(distance - skeleton.lodDistance) > skeleton.lodDistance * 0.1f)
            {
                skeleton.lodDistance = distance;
                for (LLDrawable* user : skeleton.users)
                {
                    const auto found = sObjects.find(user);
                    if (found != sObjects.end())
                    {
                        scheduleLod(user, found->second, sCameraTravel);
                    }
                }
            }
        }
        skeleton.bodyPosed |= poseSkeleton(avatar, skeleton);
        ++entry;
    }
    sChanged.insert(sAwaitingAvatar.begin(), sAwaitingAvatar.end());

    recoverBodyOnlyAvatars();

    // Entering or leaving first person rewrites what the local avatar wears (hiddenSelf()).
    static bool selfShown = true;
    if (gAgent.needsRenderAvatar() != selfShown)
    {
        selfShown = !selfShown;
        for (auto& [drawable, object] : sObjects)
        {
            if (object.bound && object.bound->avatar && object.bound->avatar->isSelf())
            {
                sChanged.insert(drawable);
            }
        }
    }
    setAnimationPeriods();
}

// Mesh arrivals, LODs and texture requests have already been serviced. System bodies
// must not create skeletons/morph sets or enqueue uploads ahead of worn mesh bodies.
void updateSystemBodies()
{
    constexpr auto BUDGET = std::chrono::microseconds(500);
    static size_t next = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t count = 0; count < sBodyAvatars.size(); ++count)
    {
        if (std::chrono::steady_clock::now() - start >= BUDGET) break;
        next = next < sBodyAvatars.size() ? next : 0;
        LLVOAvatar* avatar = sBodyAvatars[next++];
        // A body starts as soon as the avatar is built, whatever its textures and the scene's
        // backlog: waiting for a full load or a quiet scene left crowd avatars invisible.
        // Mesh work still goes first, as this runs after it in its own time slice.
        AvatarSkeleton& skeleton = skeletonOf(avatar);
        ASRAvatarBody::update(avatar, skeleton.body, skeleton.id, skeleton.bodyPosed);
        skeleton.bodyPosed = false;
    }
}

// Why an object is not drawn yet, or null when it can be.
const char* whyLeftOut(LLDrawable* drawable, LLVOVolume* volume)
{
    if (drawable->isDead() || !volume->getVolume())
    {
        return "no volume";
    }
    if (volume->isHUDAttachment() && volume->isRiggedMesh())
    {
        return "rigged HUD"; // OpenGL's HUD pass skips rigged meshes too
    }
    // OpenGL hides these and they cannot be picked: an attachment whose avatar went, kept
    // until the simulator removes it. Drawn here they stood frozen in their last pose.
    if (volume->isOrphaned() || drawable->isState(LLDrawable::FORCE_INVISIBLE))
    {
        return "orphaned";
    }
    if (volume->getAvatar() && volume->isMesh() && !volume->getSkinInfo() && !volume->isSkinInfoUnavaliable() &&
        gMeshRepo.hasSkinInfo(volume->getVolume()->getParams().getSculptID()))
    {
        return "skin to come"; // drawn unrigged it would stand at its attachment point, the size of a building
    }
    bool rigged = false;
    if (boundAvatar(volume, rigged) && skipping(rigged ? SKIP_RIGGED : SKIP_ATTACHED))
    {
        return "skipped";
    }
    if (volume->isFlexible())
    {
        return "flexible";
    }

    const U8 sculptType = volume->getVolume()->getParams().getSculptType() & LL_SCULPT_TYPE_MASK;
    if (sculptType == LL_SCULPT_TYPE_GLTF)
    {
        return "gltf";
    }
    if (sculptType != LL_SCULPT_TYPE_NONE && sculptType != LL_SCULPT_TYPE_MESH && volume->getVolume()->getSculptLevel() < -1)
    {
        return "sculpt map"; // the object is told again once its map is in
    }
    return nullptr;
}

LLVOVolume* volumeOf(LLDrawable* drawable)
{
    LLViewerObject* object = drawable && !drawable->isSpatialBridge() ? drawable->getVObj().get() : nullptr;
    return object && object->getPCode() == LL_PCODE_VOLUME ? static_cast<LLVOVolume*>(object) : nullptr;
}

// Two different surfaces in one plane tie in depth, and which one the GPU keeps would change
// as the camera moves. This settles it for good: the object's ID picks a rank, so one face
// always wins, for everyone, every session. Separate surfaces are not affected, as a rank is
// far less than a surface would need to move to change what is really in front.
constexpr U32 STACK_RANK_BITS = 3; // the low bits, for masked copies: see resolveStack()

U32 tieRankOf(const LLUUID& object, size_t face)
{
    return (U32(object.mData[0]) + U32(face)) & (asr::INSTANCE_RANK_MAX >> STACK_RANK_BITS);
}

// A rigged face names the finest LOD loaded at or below its object's own LOD, or the
// coarsest loaded when none is. Each skinned instance keeps a deformed copy sized for the
// LOD it names, so naming the shape's finest would size every far avatar in a crowd for a
// close-up. The GPU still picks within the chain from here, and never finer.
U32 skinnedGeometry(const ShapeFace& shapeFace, S32 lod)
{
    for (S32 at = llclamp(lod, 0, S32(LODS) - 1); at >= 0; --at)
    {
        if (shapeFace.geometries[at] != 0)
        {
            return shapeFace.geometries[at];
        }
    }
    for (S32 at = lod + 1; at < S32(LODS); ++at)
    {
        if (shapeFace.geometries[at] != 0)
        {
            return shapeFace.geometries[at];
        }
    }
    return shapeFace.finest;
}

// What the local avatar wears is not drawn when the viewer would not draw the avatar:
// first person, unless the avatar is set visible there, and RLV's showself. The system
// body follows the same rule in ASRAvatarBody::update().
bool hiddenSelf(const WorldObject& object)
{
    return object.bound && object.bound->avatar && object.bound->avatar->isSelf() && !gAgent.needsRenderAvatar();
}

// skinnedLod is the object's LOD for a rigged face, -1 for others.
void writeFace(ObjectFace& face, const ShapeFace& shapeFace, asr::InstanceDesc& desc, S32 skinnedLod, bool hide = false)
{
    desc.geometry = face.hidden || hide ? 0 : skinnedLod >= 0 ? skinnedGeometry(shapeFace, skinnedLod) : shapeFace.finest;
    desc.material = face.material->id;
    // Rigged layers (lipstick, eyeliner) sit microns above the skin, under what an ID rank
    // moves a face at a few metres, so the skin would show through. Exact ties go as the
    // viewer's passes draw them, each over the last at equal depth: blended over cut-out over
    // opaque, then the face order. At most 23 steps, microns at any distance.
    const U32 pass = (face.flags & asr::INSTANCE_BLENDED) ? 2 : (face.flags & asr::INSTANCE_MASKED) ? 1 : 0;
    const U32 rank = skinnedLod >= 0 ? (pass << 3) | face.tieRank : (face.tieRank << STACK_RANK_BITS) | face.rank;
    const bool hud = (face.flags & asr::INSTANCE_HUD) != 0;
    // A HUD's layers lie millimetres apart in a depth range a rank step would jump across:
    // they keep no rank, and go by draw order, as OpenGL's.
    desc.flags = face.flags | (hud ? 0 : rank << asr::INSTANCE_RANK_SHIFT);
    if (hud)
    {
        // OpenGL's alpha sort distance (LLDrawable::updateDistance()): the face's box in HUD
        // space, its centre moved a quarter of its depth toward the camera, along X.
        F32 low = F32_MAX;
        F32 high = -F32_MAX;
        for (U32 corner = 0; corner < 8; ++corner)
        {
            const F32 p[3] = { shapeFace.extents[corner & 1][0], shapeFace.extents[(corner >> 1) & 1][1], shapeFace.extents[(corner >> 2) & 1][2] };
            const F32 x = desc.objectToWorld[0] * p[0] + desc.objectToWorld[1] * p[1] + desc.objectToWorld[2] * p[2] + desc.objectToWorld[3];
            low = llmin(low, x);
            high = llmax(high, x);
        }
        desc.hudDepth = 0.5f * (low + high) - 0.25f * (high - low);
    }
    if (face.instance == NO_INSTANCE)
    {
        face.instance = sScene->addInstance(desc);
    }
    else
    {
        sScene->setInstance(face.instance, desc);
    }
}

// Exact copies of a surface tie in depth, and the order the GPU draws them in changes every
// frame, so they would flicker. Only one opaque face of a stack is drawn, the same one every
// time, which also saves drawing the rest. Masked faces can have holes, so they all stay, in
// a fixed order above it.
void resolveStack(Stack& stack)
{
    size_t faces = 0;
    for (StackMember& member : stack.members)
    {
        faces = llmax(faces, member.object->faces.size());
        member.rewrite = false;
    }

    for (size_t i = 0; i < faces; ++i)
    {
        bool covered = false;
        U32 rank = 0;
        for (StackMember& member : stack.members)
        {
            std::vector<ObjectFace>& objectFaces = member.object->faces;
            if (i >= objectFaces.size() || objectFaces[i].instance == NO_INSTANCE)
            {
                continue;
            }

            ObjectFace& face = objectFaces[i];
            const bool masked = (face.flags & (asr::INSTANCE_MASKED | asr::INSTANCE_BLENDED)) != 0; // what shows through it stays
            const bool hidden = !masked && covered;
            const U32 faceRank = masked && stack.members.size() > 1 ? llmin(++rank, (1u << STACK_RANK_BITS) - 1) : 0;
            covered = covered || !masked;
            if (hidden != face.hidden || faceRank != face.rank)
            {
                face.hidden = hidden;
                face.rank = faceRank;
                member.rewrite = true;
            }
        }
    }

    for (const StackMember& member : stack.members)
    {
        LLVOVolume* volume = member.rewrite ? volumeOf(member.drawable) : nullptr;
        if (volume)
        {
            WorldObject& object = *member.object;
            asr::InstanceDesc desc;
            fillTransform(desc, member.drawable, volume, *object.shape);
            for (size_t i = 0; i < object.faces.size(); ++i)
            {
                if (object.faces[i].instance != NO_INSTANCE)
                {
                    writeFace(object.faces[i], object.shape->faces[i], desc, -1); // stacks are never rigged
                }
            }
        }
    }
}

void leaveStack(LLDrawable* drawable, WorldObject& object)
{
    Stack* stack = object.stack;
    if (!stack)
    {
        return;
    }

    object.stack = nullptr;
    auto& members = stack->members;
    members.erase(std::find_if(members.begin(), members.end(), [drawable](const StackMember& member) { return member.drawable == drawable; }));
    if (members.empty())
    {
        sStacks.erase(object.stackKey);
    }
    else
    {
        resolveStack(*stack);
    }
}

// Costs one key and one compare, unless the object changed places.
void placeInStack(LLDrawable* drawable, WorldObject& object, const asr::InstanceDesc& desc)
{
    constexpr F32 STEPS_PER_METRE = 1024.f;
    StackKey key;
    key.shape = object.shape;
    for (U32 i = 0; i < 12; ++i)
    {
        key.matrix[i] = S32(floorf(desc.objectToWorld[i] * STEPS_PER_METRE + 0.5f));
    }

    if (!object.stack || !(key == object.stackKey))
    {
        leaveStack(drawable, object);
        object.stackKey = key;
        object.stack = &sStacks[key];
        const StackMember member{ drawable->getVObj()->getID(), drawable, &object, false };
        auto& members = object.stack->members;
        members.insert(std::upper_bound(members.begin(), members.end(), member, [](const StackMember& a, const StackMember& b) { return a.id < b.id; }), member);
    }

    // Also for an object alone in its place: it may have just come out of a stack.
    resolveStack(*object.stack);
}

// What rebuildGeom() does for every object it rebuilds, and it does not run here. Without it
// a glTF material's textures are never looked up, so its faces draw white, and overrides
// from the region's cache are never applied. Each of those that changes something, and each
// material that finishes loading, marks the object for rebuild, which comes back here.
void refreshGltfMaterials(LLVOVolume* volume)
{
    bool gltf = false;
    for (U8 te = 0; te < volume->getNumTEs(); ++te)
    {
        if (volume->getRenderMaterialID(te).notNull())
        {
            volume->updateTEMaterialTextures(te);
            gltf = true;
        }
    }
    if (gltf)
    {
        gGLTFMaterialList.applyQueuedOverrides(volume);
    }
}

// Selections. Each selected prim's faces carry flags (asr::INSTANCE_SELECTED and the rest),
// so only prims whose selection changes are applied again: see followSelection().
struct SelectionMark
{
    U32 flags = 0;  // INSTANCE_SELECTED or INSTANCE_SELECTED_CHILD
    S32 faces = 0;  // the face tool's selected faces, a bit each, as LLSelectNode keeps them
    bool operator==(const SelectionMark& other) const { return flags == other.flags && faces == other.faces; }
};
std::unordered_map<LLDrawable*, SelectionMark> sSelected;
std::unordered_map<LLDrawable*, SelectionMark> sNextSelected; // reused

U32 selectionFlags(LLDrawable* drawable, S32 face)
{
    const auto found = sSelected.find(drawable);
    if (found == sSelected.end())
    {
        return 0;
    }
    const bool faceSelected = face < 32 && (found->second.faces & (1 << face)) != 0;
    return found->second.flags | (faceSelected ? asr::INSTANCE_FACE_SELECTED : 0);
}

// As LLSelectMgr::renderSilhouettes() walks the selection: the root's colour for roots,
// the children's for the rest; in face mode only prims with a selected face, and those
// faces tinted, as LLPipeline::renderHighlights() tints them.
void followSelection()
{
    sNextSelected.clear();
    if (!LLSelectMgr::instanceExists())
    {
        return;
    }
    LLSelectMgr* manager = LLSelectMgr::getInstance();
    const bool faceMode = manager->getTEMode();
    LLObjectSelectionHandle selection = manager->getSelection();
    for (LLObjectSelection::iterator iter = selection->begin(); iter != selection->end(); ++iter)
    {
        LLSelectNode* node = *iter;
        LLViewerObject* object = node ? node->getObject() : nullptr;
        if (!object || !object->mDrawable || (faceMode && !node->hasSelectedTE()))
        {
            continue;
        }
        SelectionMark mark;
        mark.flags = object->isRootEdit() ? asr::INSTANCE_SELECTED : asr::INSTANCE_SELECTED_CHILD;
        mark.faces = faceMode ? node->getTESelectMask() : 0;
        sNextSelected[object->mDrawable.get()] = mark;
    }
    for (const auto& entry : sNextSelected)
    {
        const auto found = sSelected.find(entry.first);
        if (found == sSelected.end() || !(found->second == entry.second))
        {
            sChanged.insert(entry.first);
        }
    }
    for (const auto& entry : sSelected)
    {
        if (sNextSelected.find(entry.first) == sNextSelected.end())
        {
            sChanged.insert(entry.first); // deselected; a dead one is dropped by applyObject()
        }
    }
    sSelected.swap(sNextSelected);
}

// HUD objects, which OpenGL's calcLOD() always gives the highest LOD. One first applied
// before it counted as a HUD, at login say, took a world LOD and a check far off in camera
// travel, which a HUD never makes: a mesh then stayed at its lowest LOD.
std::unordered_set<LLDrawable*> sHudObjects;

void followHudPrim(LLViewerObject* object)
{
    LLVOVolume* volume = object && object->mDrawable.notNull() ? volumeOf(object->mDrawable.get()) : nullptr;
    if (volume && !object->mDrawable->isDead())
    {
        LLDrawable* drawable = object->mDrawable.get();
        // 3: the highest. Not updateLOD(): calcLOD() gives up on a zero distance to the
        // camera, before its HUD branch, and nothing here measures that distance for HUDs.
        if (volume->getLOD() != 3)
        {
            volume->forceLOD(3);
            sChanged.insert(drawable); // asks its mesh for the new LOD
        }
        else if (sHudObjects.find(drawable) == sHudObjects.end() && sObjects.find(drawable) != sObjects.end())
        {
            sChanged.insert(drawable); // applied before it was a HUD: as a world object
        }
    }
    if (object)
    {
        for (LLViewerObject* child : object->getChildren())
        {
            followHudPrim(child);
        }
    }
}

// The HUD points are the authority, not what was applied: an object can be applied
// before it counts as a HUD, and nothing applies it again when it becomes one.
void followHudLods()
{
    if (!isAgentAvatarValid())
    {
        return;
    }
    for (const auto& point : gAgentAvatarp->mAttachmentPoints)
    {
        if (point.second && point.second->getIsHUDAttachment())
        {
            for (const LLPointer<LLViewerObject>& object : point.second->mAttachedObjects)
            {
                followHudPrim(object.get());
            }
        }
    }
}

void applyObject(LLDrawable* drawable)
{
    LLVOVolume* volume = volumeOf(drawable);
    const char* leftOut = volume ? whyLeftOut(drawable, volume) : "no volume";
    if (leftOut)
    {
        sLeftOut[drawable] = leftOut;
    }
    else
    {
        sLeftOut.erase(drawable);
    }
    if (leftOut)
    {
        sHudObjects.erase(drawable);
        dropObject(drawable);
        return;
    }
    if (volume->isHUDAttachment())
    {
        sHudObjects.insert(drawable);
    }
    else
    {
        sHudObjects.erase(drawable);
    }

    const auto entry = sObjects.try_emplace(drawable);
    WorldObject& object = entry.first->second;
    if (entry.second)
    {
        object.sweepSlot = sSweep.size();
        sSweep.push_back({ drawable, &object });
    }
    scheduleLod(drawable, object, sCameraTravel);

    // The old shape stays in sight until the new one can take over, so a sculpt that
    // sharpens does not blink out while a worker builds it.
    const ShapeKey key = shapeKey(*volume->getVolume());
    const Shape* latest = object.next ? object.next : object.shape;
    if (!latest || !(latest->key == key))
    {
        if (object.next)
        {
            releaseShape(object.next, drawable);
            object.next = nullptr;
        }
        if (!object.shape || !(object.shape->key == key))
        {
            object.next = useShape(key, drawable);
            object.askedLod = -1;
            object.askedWithHeader = false;
        }
    }
    if (object.next && (object.next->ready || !object.shape || !object.shape->ready))
    {
        Shape* next = object.next;
        object.next = nullptr;
        clearObject(drawable, object);
        object.shape = next;
    }

    wantMeshLods(object, volume);
    if (!object.shape->ready)
    {
        return;
    }
    refreshGltfMaterials(volume);
    // rebuildGeom() also gives an animated object its own skeleton, once its mesh and skin
    // are in. Without it an animesh whose mesh came after its animations never got one: alone
    // it stood still and unskinned, worn it took the wearer's skeleton. A new skeleton marks
    // the object for rebuild, which comes back here and binds it.
    if (!volume->isMesh() || volume->getSkinInfo() || volume->isSkinInfoUnavaliable())
    {
        volume->updateControlAvatar();
    }

    const Shape& shape = *object.shape;
    const size_t faces = llmin(size_t(volume->getNumTEs()), shape.faces.size());
    for (size_t i = faces; i < object.faces.size(); ++i)
    {
        clearFace(object.faces[i], drawable);
    }
    object.faces.resize(faces);

    asr::InstanceDesc desc;
    bool rigged = false;
    if (boundAvatar(volume, rigged))
    {
        leaveStack(drawable, object); // a place in a joint's space is no place in the world
        if (!bindToAvatar(drawable, volume, object, desc))
        {
            for (ObjectFace& face : object.faces)
            {
                clearFace(face, drawable);
            }
            sAwaitingAvatar.insert(drawable);
            return;
        }
    }
    else
    {
        releaseBinding(drawable, object);
        fillTransform(desc, drawable, volume, shape);
    }
    sAwaitingAvatar.erase(drawable);
    object.skinnedLod = desc.skin != 0 ? volume->getLOD() : -1;
    for (size_t i = 0; i < faces; ++i)
    {
        ObjectFace& face = object.faces[i];
        asr::MaterialDesc material;
        LLViewerTexture* image = nullptr;
        U32 flags = 0;
        if (shape.faces[i].finest == 0 || !skinnable(shape.faces[i], object) || !fillMaterial(material, flags, image, volume, S32(i), shape.faces[i].box))
        {
            clearFace(face, drawable);
            continue;
        }

        // The new one comes first, so a material that stays is not made again, and the
        // old one goes last, so no instance is left pointing at it.
        SharedMaterial* previous = face.material;
        face.material = useMaterial(material, image, drawable);
        face.flags = flags | (volume->isHUDAttachment() ? asr::INSTANCE_HUD : 0) | selectionFlags(drawable, S32(i));
        face.tieRank = object.skinnedLod >= 0 ? U32(llmin(i, size_t(7))) : tieRankOf(drawable->getVObj()->getID(), i);
        writeFace(face, shape.faces[i], desc, object.skinnedLod, hiddenSelf(object));
        if (previous)
        {
            releaseMaterial(previous, drawable);
        }
    }
    if (!object.bound)
    {
        placeInStack(drawable, object, desc);
    }
}

void moveObject(LLDrawable* drawable)
{
    auto found = sObjects.find(drawable);
    LLVOVolume* volume = volumeOf(drawable);
    if (found == sObjects.end() || !volume || !found->second.shape || !found->second.shape->ready)
    {
        return;
    }

    WorldObject& object = found->second;
    scheduleLod(drawable, object, sCameraTravel); // the camera is not all that changes distance
    asr::InstanceDesc desc;
    if (object.bound)
    {
        // Worn: it moves with its avatar every frame, and that is the skeleton's work. Only
        // an attachment's place against its joint is its own.
        if (object.bound->skin)
        {
            return;
        }
        LLViewerJointAttachment* point = object.bound->avatar->getTargetAttachmentPoint(volume->getRootEdit());
        F32 local[12];
        if (point && U32(point->getJointNum()) == object.bound->joint)
        {
            attachedMatrix(drawable, volume, point, local);
            if (memcmp(local, object.bound->local, sizeof(local)) == 0)
            {
                return;
            }
        }
        if (!bindToAvatar(drawable, volume, object, desc))
        {
            sChanged.insert(drawable);
            return;
        }
    }
    else
    {
        fillTransform(desc, drawable, volume, *object.shape);
        placeInStack(drawable, object, desc);
    }
    for (size_t i = 0; i < object.faces.size(); ++i)
    {
        ObjectFace& face = object.faces[i];
        if (face.instance != NO_INSTANCE && !face.hidden)
        {
            // The LOD it was applied at: a move never rebinds to another size.
            writeFace(face, object.shape->faces[i], desc, desc.skin != 0 ? object.skinnedLod : -1, hiddenSelf(object));
        }
    }
}

void markMoved(LLViewerObject* object)
{
    if (object->mDrawable.notNull() && sObjects.count(object->mDrawable.get()) != 0)
    {
        sMoved.insert(object->mDrawable.get());
    }
    for (LLViewerObject* child : object->getChildren())
    {
        markMoved(child);
    }
}

void finishTerrainPatch(TerrainJob& job);

void applyFinishedShapes()
{
    std::vector<ShapeJob> finished;
    {
        std::lock_guard<std::mutex> lock(sJobMutex);
        finished.swap(sFinished);
    }
    for (ShapeJob& job : finished)
    {
        if (job.terrain)
        {
            finishTerrainPatch(*job.terrain);
            continue;
        }
        Shape* shape = job.shape;
        // A mesh face can be empty at a coarse LOD and gain triangles at a finer one: it had no
        // instance, which a move does not make, so its users go through apply again.
        bool newFaces = !shape->ready || job.faces.size() != shape->faces.size();
        for (size_t i = 0; !newFaces && i < job.faces.size(); ++i)
        {
            newFaces = shape->faces[i].finest == 0 && job.faces[i].finest != 0;
        }
        shape->busy = false;
        shape->ready = true;
        shape->faces = std::move(job.faces);
        shape->lodBias = job.lodBias;
        if (shape->users.empty())
        {
            forgetShape(shape);
            continue;
        }
        if (isMeshKey(shape->key))
        {
            startNextMeshLod(*shape);
        }

        // Instances name the finest LOD. A finer one changes only that, so a mesh repeated
        // thousands of times does not go through all its materials again.
        // A worn one is not moved by the world, so it goes through apply, which rebinds it.
        for (LLDrawable* user : shape->users)
        {
            const auto found = sObjects.find(user);
            (newFaces || (found != sObjects.end() && found->second.bound) ? sChanged : sMoved).insert(user);
        }
    }
}

// Applying is the costly part of a burst, such as the first LOD of a mesh with thousands of
// copies, so it gets a share of each frame and the rest waits for the next one.
void applyChanges()
{
    constexpr auto BUDGET = std::chrono::milliseconds(4);
    constexpr U32 CHECK_EVERY = 16; // reading the clock is not free either

    const auto start = std::chrono::steady_clock::now();
    U32 applied = 0;
    for (auto next = sChanged.begin(); next != sChanged.end();)
    {
        LLDrawable* drawable = *next;
        next = sChanged.erase(next);
        sMoved.erase(drawable); // applying it writes its transform too
        applyObject(drawable);
        if (++applied % CHECK_EVERY == 0 && std::chrono::steady_clock::now() - start >= BUDGET)
        {
            return;
        }
    }
}

// Drop only fine LODs that no live user has requested within the residency window.
// SceneStore already keeps removing geometry alive until every instance and finer LOD
// releases it; unrelated object-update backlog must not prevent retirement.
void evictUnusedMeshLods()
{
    constexpr auto BUDGET = std::chrono::microseconds(500);
    static ShapeKey cursor;
    static size_t remaining = 0;
    static F32 lastSweep = -1000.f;
    if (sShapes.empty()) return;
    if (remaining == 0)
    {
        if (gFrameTimeSeconds - lastSweep < 1.f) return;
        remaining = sShapes.size();
        lastSweep = gFrameTimeSeconds;
    }
    const auto start = std::chrono::steady_clock::now();
    const bool pressure = memoryPressure();
    auto next = sShapes.upper_bound(cursor);
    while (remaining != 0)
    {
        if (next == sShapes.end()) next = sShapes.begin();
        auto entry = next++;
        cursor = entry->first;
        --remaining;
        Shape& shape = *entry->second;
        if (isMeshKey(shape.key) && !shape.busy && shape.ready)
        {
            U32 top = 0;
            for (const ShapeFace& face : shape.faces)
                for (U32 lod = 1; lod < LODS; ++lod)
                    if (face.geometries[lod] != 0) top = llmax(top, lod);

            bool replaceable = top != 0 && !shape.residency.holds(top, gFrameTimeSeconds, pressure);
            if (replaceable && meshLodNeeded(shape, top)) replaceable = false;
            for (const ShapeFace& face : shape.faces)
            {
                if (!replaceable || face.geometries[top] == 0) continue;
                bool coarser = false;
                for (U32 lod = 0; lod < top; ++lod)
                    coarser |= face.geometries[lod] != 0 && (face.skinJoints[top] == 0 ||
                        (face.skinJoints[lod] != 0 && face.skinJoints[lod] <= face.skinJoints[top]));
                // Empty, unweighted or incompatible coarse faces must not remove an outfit.
                replaceable = coarser;
            }
            if (replaceable)
            {
                for (ShapeFace& face : shape.faces)
                {
                    if (face.geometries[top] == 0) continue;
                    sScene->removeGeometry(face.geometries[top]);
                    face.geometries[top] = 0;
                    face.skinJoints[top] = 0;
                    face.finest = 0;
                    for (U32 lod = 0; lod < top; ++lod)
                        if (face.geometries[lod] != 0) face.finest = face.geometries[lod];
                }
                shape.nextLod = top;
                for (U32 lod = top; lod < LODS; ++lod) shape.arrived[lod] = nullptr;
                for (LLDrawable* user : shape.users)
                {
                    auto object = sObjects.find(user);
                    if (object != sObjects.end()) object->second.askedLod = llmin(object->second.askedLod, S32(top) - 1);
                    sChanged.insert(user);
                }
            }
        }
        if (std::chrono::steady_clock::now() - start >= BUDGET) break;
    }
}

// The viewer walks all its textures in ID order, within a few milliseconds per loop, and
// knows nothing about what is in view. With tens of thousands of them, what just came into
// view waits up to half a minute for its turn. So textures in view get their turn here, as
// often as the sweep comes round, however many textures the viewer holds.
void visitTexture(TrackedTexture& texture)
{
    LLViewerFetchedTexture* fetched = LLViewerTextureManager::staticCastToFetchedTexture(texture.image.get());
    if (fetched && fetched->getGLTexture())
    {
        gTextureList.updateImageDecodePriority(fetched, false);
        fetched->updateFetch();
    }
}

// A face with no image at all comes first, as a blurry surface beats an empty one. Textures
// that only want to get sharper wait in sSharper for what is left of the sweep's time.
void visitTextures(const WorldObject& object)
{
    for (const ObjectFace& face : object.faces)
    {
        TrackedTexture* texture = face.material && !face.hidden ? face.material->texture : nullptr;
        if (!texture || gFrameTimeSeconds - texture->visited < LOD_SWEEP_SECONDS)
        {
            continue;
        }
        texture->visited = gFrameTimeSeconds;
        if (texture->slot == 0)
        {
            visitTexture(*texture);
        }
        else
        {
            sSharper.push_back(texture);
        }
    }
}

// Largest on screen first. What does not fit is visited again by the next sweep.
void visitSharperTextures()
{
    constexpr auto BUDGET = std::chrono::milliseconds(1);

    std::sort(sSharper.begin(), sSharper.end(), [](TrackedTexture* a, TrackedTexture* b) { return a->image->getMaxVirtualSize() > b->image->getMaxVirtualSize(); });
    const auto start = std::chrono::steady_clock::now();
    for (TrackedTexture* texture : sSharper)
    {
        if (std::chrono::steady_clock::now() - start < BUDGET)
        {
            visitTexture(*texture);
        }
        else
        {
            texture->visited = -1000.f;
        }
    }
    sSharper.clear();
}

// What calcLOD() does to distance and to the LOD factor, as one number: distance / object
// size is multiplied by this before it is compared with COARSER_AT. Its boost very close up
// is left out here, as that only ever chooses the finest LOD earlier.
F32 lodScale()
{
    F32 factor = LLVOVolume::sLODFactor;
    static LLCachedControl<bool> ignore_fov_zoom(gSavedSettings, "IgnoreFOVZoomForLODs");
    if (!ignore_fov_zoom)
    {
        factor *= DEFAULT_FIELD_OF_VIEW / LLViewerCamera::getInstance()->getDefaultFOV();
    }
    return (F_PI / 3.f) * LLVOVolume::sDistanceFactor / llmax(factor, 0.001f);
}

// How far the camera must travel before this object's LOD can possibly change: the way to
// the nearest distance where it would. Close up, calcLOD() bends distance, so it is simply
// looked at again soon.
F32 metresToNextLod(LLVOVolume* volume, F32 distance, F32 scale)
{
    constexpr F32 SMALLEST = 0.25f;
    constexpr F32 CLOSE_UP_STEP = 0.5f;
    if (distance * LLVOVolume::sDistanceFactor < LLVOVolume::sLODFactor * 2.f)
    {
        return CLOSE_UP_STEP;
    }

    F32 radius = volume->getVolume() ? volume->getVolume()->mLODScaleBias.scaledVec(volume->getScale()).length() : volume->getScale().length();
    bool rigged = false;
    LLVOAvatar* avatar = boundAvatar(volume, rigged);
    if (rigged)
    {
        const LLVector3* extents = avatar->getLastAnimExtents();
        radius = (extents[1] - extents[0]).length() * (avatar->isControlAvatar() ? 0.5f : 1.f);
    }
    F32 metres = F32_MAX;
    for (U32 lod = 1; lod < LODS; ++lod)
    {
        metres = llmin(metres, fabsf(distance - COARSER_AT[lod] * radius / llmax(scale, 0.001f)));
    }
    return llmax(metres, SMALLEST);
}

// The viewer works out each object's LOD in updateCull(), which does not run here. For a
// mesh that LOD is what gets loaded, and it is also what picking tests against. The GPU
// picks what it draws by itself, from the same rule.
//
// An object's LOD can only change once the camera has come a known distance closer or
// further, so each object waits in a queue for that much travel. A still camera costs nothing
// here, however many objects there are, and what it costs in motion follows the motion. Objects
// behind the camera are kept up to date too, so turning around shows them at the right LOD.
void updateLods()
{
    constexpr auto BUDGET = std::chrono::milliseconds(1);
    constexpr U32 CHECK_EVERY = 16;

    const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3 renderEye = eye + sRenderOrigin; // a crossing moves agent space, not the camera
    sCameraTravel += (renderEye - sLastEye).length();
    sLastEye = renderEye;

    // A new LOD factor or field of view moves every object's limits.
    const F32 scale = lodScale();
    if (scale != sLastLodScale)
    {
        sLastLodScale = scale;
        for (const SweepEntry& entry : sSweep)
        {
            scheduleLod(entry.drawable, *entry.object, sCameraTravel);
        }
    }

    const auto start = std::chrono::steady_clock::now();
    U32 checked = 0;
    while (!sLodChecks.empty() && sLodChecks.top().due <= sCameraTravel)
    {
        const LodCheck check = sLodChecks.top();
        sLodChecks.pop();
        auto found = sObjects.find(check.drawable);
        if (found == sObjects.end() || found->second.lodDue != check.due)
        {
            continue;
        }

        WorldObject& object = found->second;
        object.lodDue = F64_MAX;
        LLVOVolume* volume = volumeOf(check.drawable);
        if (volume && !check.drawable->isDead())
        {
            bool rigged = false;
            LLVOAvatar* avatar = boundAvatar(volume, rigged);
            // A HUD is in HUD space, at the screen: always its finest detail.
            const F32 distance = volume->isHUDAttachment() ? 0.f :
                                 rigged && avatar->mDrawable.notNull() ? avatar->mDrawable->mDistanceWRTCamera :
                                                                         (check.drawable->getPositionAgent() - eye).length();
            check.drawable->mDistanceWRTCamera = ll_round(distance, 0.01f);
            volume->updateLOD();
            scheduleLod(check.drawable, object, sCameraTravel + metresToNextLod(volume, distance, scale));
            // A rigged face's deformed copy is sized for the LOD it names: see skinnedGeometry().
            if (object.skinnedLod >= 0 && volume->getLOD() != object.skinnedLod)
            {
                sChanged.insert(check.drawable);
            }
        }
        if (++checked % CHECK_EVERY == 0 && std::chrono::steady_clock::now() - start >= BUDGET)
        {
            return;
        }
    }
}

// What has to happen per object in view, or until it is settled.
void sweepObjects()
{
    if (sSweep.empty())
    {
        return;
    }

    // A slow frame asks for a larger share, which is the worst time to spend it, so this
    // has a limit of its own. The sweep then just takes longer to come round.
    constexpr auto BUDGET = std::chrono::milliseconds(1);
    constexpr size_t CHECK_EVERY = 16;

    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const size_t count = llclamp(size_t(F32(sSweep.size()) * gFrameIntervalSeconds.value() / LOD_SWEEP_SECONDS) + 1, size_t(1), sSweep.size());
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < count; ++i)
    {
        if (i % CHECK_EVERY == CHECK_EVERY - 1 && std::chrono::steady_clock::now() - start >= BUDGET)
        {
            break;
        }

        sSweepNext = sSweepNext < sSweep.size() ? sSweepNext : 0;
        const SweepEntry entry = sSweep[sSweepNext++];
        LLVOVolume* volume = volumeOf(entry.drawable);
        if (!volume || entry.drawable->isDead())
        {
            continue;
        }

        // Worn meshes can retain bind-space drawable bounds without GL batching.
        // Their first textures must follow the live wearer, like animation priority.
        LLVOAvatar* avatar = entry.object->bound ? entry.object->bound->avatar : nullptr;
        const LLVector3 center = avatar ? avatar->getRenderPosition() : entry.drawable->getPositionAgent();
        const LLVector3* extents = avatar ? avatar->getLastAnimExtents() : nullptr;
        const F32 radius = extents ? llmax((extents[1] - extents[0]).length() * 0.5f, 1.5f) : entry.drawable->getRadius();
        if (camera->sphereInFrustum(center, radius))
        {
            visitTextures(*entry.object);
        }
        wantMeshLods(*entry.object, volume);
    }
    visitSharperTextures();
}

// A texture costs a slot however small it is, and a busy place can show more textures than
// there are slots. With slots to spare nothing goes, so turning around stays sharp. Past that
// the time out of view allowed falls as slots run out, from minutes to seconds. It steers
// itself: what has been out of view longest goes first, and only as much as is needed, as
// each release moves this limit back up.
void updateUnseenLimit()
{
    constexpr U32 SLOTS_TO_KEEP = 8192;
    constexpr F32 LONGEST = 120.f;
    constexpr F32 SHORTEST = 2.f;

    const U32 free = ASR::freeTextureSlots();
    sUnseenLimit = free >= SLOTS_TO_KEEP ? F32_MAX : LONGEST * powf(SHORTEST / LONGEST, 1.f - F32(free) / F32(SLOTS_TO_KEEP));
}

// A texture keeps its slot when it is loaded sharper or shrunk: the renderer swaps the image
// behind it once the new one is filled. A new slot, then, is a first image or a lost one,
// and only touches the materials on that texture. An alpha channel can appear, which can
// change how its faces draw, so those go through applyObject() again.
void followTextures()
{
    // Rare: the default image is one texture, and only moves if it is replaced.
    const U32 defaultSlot = defaultImageSlot();
    if (defaultSlot != sDefaultSlot)
    {
        sDefaultSlot = defaultSlot;
        for (const auto& texture : sTextures)
        {
            if (texture.second.slot == 0)
            {
                writeMaterials(texture.second);
            }
        }
    }

    ASR::takeChangedImages(sChangedImages);
    for (const void* image : sChangedImages)
    {
        auto found = sTextures.find(image);
        if (found == sTextures.end())
        {
            continue;
        }

        TrackedTexture& texture = found->second;
        const S8 components = texture.image->getComponents();
        if (components != texture.components)
        {
            texture.components = components;
            for (const auto& user : texture.users)
            {
                sChanged.insert(user.first);
            }
        }

        const U32 slot = ASR::textureSlot(texture.image->getTexName());
        if (slot != texture.slot)
        {
            ++texture.changes;
            if (slot == 0 && texture.image->getDiscardLevel() >= 0 && ++sLostTextures <= 5) // released ones have no discard level
            {
                LL_INFOS("ASRenderer") << "texture lost its image: " << texture.image->getID() << " on " << texture.users.size() << " objects, discard "
                                       << texture.image->getDiscardLevel() << ", size " << texture.image->getMaxVirtualSize() << ", bias "
                                       << LLViewerTexture::sDesiredDiscardBias << LL_ENDL;
            }
            ownSlot(texture, slot);
            writeMaterials(texture);
        }
    }
}

// Terrain: each region's ground is its surface patches, 16 grid cells square, and each patch is
// a geometry of its own with four LODs that keep every point of their edges, so neighbours
// meet exactly at any LODs (asr::buildTerrainLod()). All patches of a region are packed in one
// box, in region metres, under one translation, so their shared edge points are the same bits.
// The region's material names its terrain entry, the four layers and how they blend, which is
// worked out here every loop, as the draw pool does, and sent when it changes.
//
// Every region LLWorld lists is watched here, each loop. Its layers are tracked from the start,
// so their textures load whether or not its heights are in. A patch whose heights have not
// come shows the surface's heights as they stand, flat at 0 with edges its neighbours wrote,
// in the region's first layer, so ground is never empty and always continuous.
// Once its heights are in and the viewer has generated its composition, it is built for real,
// and again whenever its data changes: LLSurfacePatch::dirtyZ() stamps a new update time.
// A patch whose heights change also rebuilds its built neighbours, whose edge normals read
// it. The viewer's geometry rebuilds that follow the camera's distance are not needed, as the
// GPU picks the LOD. A region that leaves the list takes all its patches with it.

// LLSurfacePatch::updateVisibility(): a grid step is used from step / 0.15 grid cells away. An
// instance's LOD size here is its grid spacing times lodScale(), so the LOD factor and field
// of view change nothing, as for the viewer's terrain.
constexpr F32 TERRAIN_COARSER_AT[LODS] = { 0.f, 2.f / 0.15f, 4.f / 0.15f, 8.f / 0.15f };
constexpr F32 TERRAIN_BOX_MARGIN = 32.f; // metres above and below the heights, for edits

} // namespace

// Reads a region's layers as LLDrawPoolTerrain does. A friend of LLTerrainMaterials.
class ASRTerrainAccess
{
public:
    static LLViewerFetchedTexture* detailTexture(LLTerrainMaterials& materials, S32 index) { return materials.mDetailTextures[index].get(); }
    static LLFetchedGLTFMaterial* renderMaterial(LLTerrainMaterials& materials, S32 index) { return materials.mDetailRenderMaterials[index].get(); }
};

namespace
{

struct TerrainJob
{
    LLSurface* surface = nullptr;   // with handle, says which region: it may be gone when the job comes back
    U64 handle = 0;
    U32 index = 0;                  // the patch, x + y * patches per edge
    U32 number = 0;                 // which build of it this is
    U32 boxVersion = 0;
    bool flat = false;              // a stand-in: one LOD is enough
    asr::GeometryBox box{};
    asr::TerrainGrid grid;          // pointing into the vectors below
    std::vector<F32> heights;
    std::vector<F32> normals;
    std::vector<F32> blends;
    U32 geometries[LODS] = { 0, 0, 0, 0 }; // finest first
};

struct TerrainPatch
{
    LLVector3 centre;               // region metres
    U32 geometries[LODS] = { 0, 0, 0, 0 };
    U32 instance = NO_INSTANCE;
    U32 material = 0;               // the region's material for the box its geometry is in
    U32 job = 0;                    // the build it waits for
    U64 key = 0;                    // the update time of the data last sent to be built; 0 asks for a build
    U64 seen = 0;                   // the update time last told to its neighbours
    bool real = false;              // built, or being built, from its own heights
    bool busy = false;
    bool again = false;             // asked for again while it was being built
};

constexpr U32 TERRAIN_IMAGES = asr::TERRAIN_LAYERS + 1; // the layers, then the alpha ramp

struct TerrainRegion
{
    LLViewerRegion* region = nullptr; // used only while LLWorld lists it
    U64 handle = 0;
    LLVector3 agent;                  // its origin in agent space, kept up with shifts
    F32 spacing = 1.f;                // metres between grid points
    U32 perEdge = 0;                  // patches along an edge
    U32 terrain = 0;
    asr::TerrainDesc desc;
    LLViewerTexture* images[TERRAIN_IMAGES] = {};
    TrackedTexture* textures[TERRAIN_IMAGES] = {};
    U32 boxVersion = 0;
    asr::GeometryBox box{};
    U32 material = 0;                           // for the current box
    std::unordered_map<U32, U32> materialUsers; // material id, and the patches drawn with it
    std::vector<TerrainPatch> patches;
    bool toldPaintMap = false;
};

std::unordered_map<LLSurface*, TerrainRegion> sTerrainRegions;
U32 sTerrainJobs = 0;
U32 sTerrainBuilt = 0;   // temporary: since the last log
U32 sTerrainReboxed = 0; // temporary: since the last log

// Worker: every LOD, coarsest first, so the chain only grows at its fine end.
void buildTerrainPatch(TerrainJob& job)
{
    std::vector<asr::SceneVertex> vertices;
    std::vector<uint16_t> indices;
    U32 coarser = 0;
    for (S32 lod = S32(LODS) - 1; lod >= 0; --lod)
    {
        const U32 step = 1u << lod;
        if (step * 2 > job.grid.size)
        {
            continue; // a patch too small for this step
        }
        asr::buildTerrainLod(job.grid, step, vertices, indices);
        coarser = sScene->addGeometry(asr::buildGeometry(vertices.data(), U32(vertices.size()), indices.data(), U32(indices.size()), &job.box),
                                      coarser, coarser != 0 ? TERRAIN_COARSER_AT[lod + 1] : 0.f);
        job.geometries[lod] = coarser;
        if (job.flat)
        {
            break; // flat ground looks the same at any step
        }
    }
}

// The finest LOD there is, which instances name.
U32 finestTerrainGeometry(const U32 (&geometries)[LODS])
{
    for (U32 id : geometries)
    {
        if (id != 0)
        {
            return id;
        }
    }
    return 0;
}

void removeTerrainGeometries(const U32 (&geometries)[LODS])
{
    for (U32 id : geometries)
    {
        if (id != 0)
        {
            sScene->removeGeometry(id);
        }
    }
}

void releaseTerrainMaterial(TerrainRegion& region, U32 material)
{
    auto found = region.materialUsers.find(material);
    if (found != region.materialUsers.end() && --found->second == 0 && material != region.material)
    {
        sScene->removeMaterial(material);
        region.materialUsers.erase(found);
    }
}

// One box for every patch of the surface, from all its heights, with room for edits. A new
// one gets its own material, and each patch moves to it as its geometry is rebuilt in it.
void boxTerrainRegion(TerrainRegion& region, LLSurface* surface)
{
    const S32 points = surface->getGridsPerEdge();
    F32 low = F32_MAX;
    F32 high = -F32_MAX;
    for (S32 k = 0; k < points * points; ++k)
    {
        low = llmin(low, surface->getZ(k));
        high = llmax(high, surface->getZ(k));
    }
    const F32 width = F32(points - 1) * surface->getMetersPerGrid();
    region.box.center[0] = width * 0.5f;
    region.box.center[1] = width * 0.5f;
    region.box.center[2] = (low + high) * 0.5f;
    region.box.scale = llmax(width * 0.5f, (high - low) * 0.5f + TERRAIN_BOX_MARGIN);
    ++region.boxVersion;

    const U32 previous = region.material;
    asr::MaterialDesc material;
    material.terrain = region.terrain;
    for (U32 axis = 0; axis < 3; ++axis)
    {
        material.planarScale[axis] = region.box.scale;
        material.planarOffset[axis] = region.box.center[axis];
    }
    region.material = sScene->addMaterial(material);
    region.materialUsers[region.material] = 0;
    if (previous != 0 && region.materialUsers[previous] == 0)
    {
        sScene->removeMaterial(previous);
        region.materialUsers.erase(previous);
    }
}

void writeTerrainInstance(const TerrainRegion& region, TerrainPatch& state)
{
    const U32 finest = finestTerrainGeometry(state.geometries);
    if (finest == 0)
    {
        return;
    }
    asr::InstanceDesc desc;
    desc.geometry = finest;
    desc.material = state.material;
    const LLVector3 origin = region.agent + sRenderOrigin;
    const F32 rows[12] = { 1.f, 0.f, 0.f, origin.mV[VX], 0.f, 1.f, 0.f, origin.mV[VY], 0.f, 0.f, 1.f, origin.mV[VZ] };
    std::copy(rows, rows + 12, desc.objectToWorld);
    for (U32 axis = 0; axis < 3; ++axis)
    {
        desc.lodCenter[axis] = origin.mV[axis] + state.centre.mV[axis];
    }
    desc.lodSize = lodScale() * region.spacing;
    if (state.instance == NO_INSTANCE)
    {
        state.instance = sScene->addInstance(desc);
    }
    else
    {
        sScene->setInstance(state.instance, desc);
    }
}

// Its heights are in, and the viewer has generated its composition from them, after its normals.
bool terrainPatchReady(const LLSurfacePatch* patch)
{
    return patch->getHasReceivedData() && patch->isHeightsGenerated() && patch->getDataZ();
}

// Sends a patch to be built, from the surface's heights as they stand, as OpenGL does: before
// its own come they are 0, flat, except along edges a neighbour with data has written, so the
// ground is continuous whatever has arrived. Its blend waits for its composition. One that is
// being built is asked for again once it is back.
void startTerrainPatch(TerrainRegion& region, LLSurface* surface, U32 index)
{
    TerrainPatch& state = region.patches[index];
    if (state.busy)
    {
        state.again = true;
        return;
    }
    LLSurfacePatch* patch = surface->getPatch(S32(index % region.perEdge), S32(index / region.perEdge));
    LLViewerRegion* regionp = region.region;
    const bool real = terrainPatchReady(patch);

    // As LLSurfacePatch::eval() gives them to the viewer's terrain.
    auto job = std::make_shared<TerrainJob>();
    const U32 size = U32(surface->getGridsPerPatchEdge());
    const U32 points = size + 1;
    const U32 stride = U32(surface->getGridsPerEdge());
    const LLVector3 origin = patch->getOriginAgent() - surface->getOriginAgent();
    const LLVector3d& global = patch->getOriginGlobal();
    const F32 noiseScale = (1.f / (4.9215f * 7.f)) * 0.2222222222f;
    const F32* z = patch->getDataZ();
    job->heights.resize(points * points);
    job->normals.resize(points * points * 3);
    job->blends.resize(points * points * 2);
    F32 low = F32_MAX;
    F32 high = -F32_MAX;
    for (U32 y = 0; y < points; ++y)
    {
        for (U32 x = 0; x < points; ++x)
        {
            const U32 at = y * points + x;
            job->heights[at] = z[x + y * stride];
            low = llmin(low, job->heights[at]);
            high = llmax(high, job->heights[at]);
            const LLVector3& normal = patch->getNormal(x, y); // the surface starts with them all up
            std::copy(normal.mV, normal.mV + 3, &job->normals[at * 3]);
            job->blends[at * 2] = real ? regionp->getCompositionXY(llfloor(origin.mV[VX]) + S32(x), llfloor(origin.mV[VY]) + S32(y)) : 0.f;
            F32 at_noise[3] = { (F32)fmod((F32)(global.mdV[VX] + x) * noiseScale, 256.f), (F32)fmod((F32)(global.mdV[VY] + y) * noiseScale, 256.f), 0.f };
            job->blends[at * 2 + 1] = llclamp(noise2(at_noise) * 0.75f + 0.5f, 0.f, 1.f);
        }
    }

    // Heights past the box ask for a new one, and every patch of the region is built again in it.
    if (low < region.box.center[2] - region.box.scale || high > region.box.center[2] + region.box.scale)
    {
        boxTerrainRegion(region, surface);
        ++sTerrainReboxed;
        for (U32 other = 0; other < region.patches.size(); ++other)
        {
            if (other != index && (finestTerrainGeometry(region.patches[other].geometries) != 0 || region.patches[other].busy))
            {
                startTerrainPatch(region, surface, other);
            }
        }
    }

    const F32 half = F32(size) * region.spacing * 0.5f;
    state.centre.setVec(origin.mV[VX] + half, origin.mV[VY] + half, (low + high) * 0.5f);
    state.busy = true;
    state.real = real;
    state.key = real ? patch->getLastUpdateTime() : 0;
    state.job = ++sTerrainJobs;

    job->surface = surface;
    job->handle = region.handle;
    job->index = index;
    job->number = state.job;
    job->boxVersion = region.boxVersion;
    job->flat = !real;
    job->box = region.box;
    job->grid.size = size;
    job->grid.origin[0] = origin.mV[VX];
    job->grid.origin[1] = origin.mV[VY];
    job->grid.spacing = region.spacing;
    job->grid.heights = job->heights.data();
    job->grid.normals = job->normals.data();
    job->grid.blends = job->blends.data();

    ShapeJob work;
    work.terrain = std::move(job);
    std::lock_guard<std::mutex> lock(sJobMutex);
    sJobs.push_back(std::move(work));
    sJobReady.notify_one();
}

void finishTerrainPatch(TerrainJob& job)
{
    auto region = sTerrainRegions.find(job.surface);
    if (region == sTerrainRegions.end() || region->second.handle != job.handle || job.index >= region->second.patches.size() ||
        region->second.patches[job.index].job != job.number)
    {
        removeTerrainGeometries(job.geometries); // its region went, or asked for a newer build
        return;
    }
    TerrainRegion& owner = region->second;
    TerrainPatch& state = owner.patches[job.index];
    state.busy = false;
    if (job.boxVersion != owner.boxVersion)
    {
        removeTerrainGeometries(job.geometries); // built in a box the region has left
        state.again = true;
    }
    else
    {
        U32 previous[LODS];
        std::copy(std::begin(state.geometries), std::end(state.geometries), previous);
        std::copy(std::begin(job.geometries), std::end(job.geometries), state.geometries);
        const U32 previousMaterial = state.material;
        state.material = owner.material;
        ++owner.materialUsers[state.material];
        writeTerrainInstance(owner, state);
        removeTerrainGeometries(previous);
        if (previousMaterial != 0)
        {
            releaseTerrainMaterial(owner, previousMaterial);
        }
        ++sTerrainBuilt;
    }
    if (state.again && LLWorld::getInstance()->isRegionListed(owner.region))
    {
        state.again = false;
        startTerrainPatch(owner, &owner.region->getLand(), job.index);
    }
}

void releaseTerrainImages(TerrainRegion& region)
{
    for (U32 i = 0; i < TERRAIN_IMAGES; ++i)
    {
        if (region.textures[i])
        {
            --region.textures[i]->terrainUsers;
            forgetUnusedTexture(region.textures[i]);
        }
        region.textures[i] = nullptr;
        region.images[i] = nullptr;
    }
}

// A build still running finds no region, and lets its geometry go.
void dropTerrainRegion(TerrainRegion& region)
{
    for (const TerrainPatch& state : region.patches)
    {
        if (state.instance != NO_INSTANCE)
        {
            sScene->removeInstance(state.instance);
        }
        removeTerrainGeometries(state.geometries);
    }
    for (const auto& material : region.materialUsers)
    {
        sScene->removeMaterial(material.first);
    }
    sScene->removeTerrain(region.terrain);
    releaseTerrainImages(region);
}

// The layers as LLDrawPoolTerrain::renderFullShader() binds them.
void refreshTerrainLayers(TerrainRegion& region)
{
    static LLCachedControl<F32> terrain_scale(gSavedSettings, "RenderTerrainScale");
    static LLCachedControl<F32> pbr_scale(gSavedSettings, "RenderTerrainPBRScale");
    static LLCachedControl<S32> pbr_planes(gSavedSettings, "RenderTerrainPBRPlanarSampleCount");
    static LLCachedControl<F32> pbr_sharpness(gSavedSettings, "RenderTerrainPBRTriplanarBlendFactor");
    static LLPointer<LLViewerFetchedTexture> ramp = LLViewerTextureManager::getFetchedTexture(IMG_ALPHA_GRAD_2D);

    LLVLComposition* composition = region.region->getComposition();
    if (!composition)
    {
        return;
    }
    composition->boost(); // as the draw pool does every time it draws

    const bool local = gLocalTerrainMaterials.makeMaterialsReady(true, false);
    const bool textures = !local && composition->getMaterialType() == LLTerrainMaterials::Type::TEXTURE;
    LLTerrainMaterials& source = local ? gLocalTerrainMaterials : *composition;

    asr::TerrainDesc desc;
    LLViewerTexture* images[TERRAIN_IMAGES] = {};
    if (textures)
    {
        // Detail textures lie flat, at RenderTerrainScale metres, continuing across regions.
        const F32 scale = 1.f / llmax(F32(terrain_scale), 0.001f);
        const LLVector3d origin = region.region->getOriginGlobal();
        const F32 offset[2] = { (F32)fmod(origin.mdV[VX], 1.0 / (F64)scale) * scale, (F32)fmod(origin.mdV[VY], 1.0 / (F64)scale) * scale };
        desc.kind = asr::TERRAIN_TEXTURES;
        for (U32 i = 0; i < asr::TERRAIN_LAYERS; ++i)
        {
            images[i] = ASRTerrainAccess::detailTexture(*composition, S32(i));
            asr::TerrainLayerDesc& layer = desc.layers[i];
            layer.uvRow0[0] = scale;
            layer.uvRow0[2] = offset[0];
            layer.uvRow1[1] = scale;
            layer.uvRow1[2] = offset[1];
        }
    }
    else
    {
        // Base colours, through each material's transform as glTF defines it, with V
        // flipped in and out, and RenderTerrainPBRScale folded into its scale.
        const F32 scale = 1.f / llmax(F32(pbr_scale), 0.001f);
        desc.kind = asr::TERRAIN_MATERIALS;
        desc.planes = pbr_planes == 3 ? 3 : 1;
        desc.sharpness = pbr_sharpness;
        for (U32 i = 0; i < asr::TERRAIN_LAYERS; ++i)
        {
            LLFetchedGLTFMaterial* fetched = ASRTerrainAccess::renderMaterial(source, S32(i));
            const LLGLTFMaterial& material = fetched ? *fetched : LLGLTFMaterial::sDefault;
            images[i] = fetched ? fetched->mBaseColorTexture.get() : nullptr;
            const LLGLTFMaterial::TextureTransform& transform = material.mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
            const F32 c = cosf(transform.mRotation);
            const F32 s = sinf(transform.mRotation);
            const F32 scaleU = transform.mScale.mV[VX] * scale;
            const F32 scaleV = transform.mScale.mV[VY] * scale;
            asr::TerrainLayerDesc& layer = desc.layers[i];
            layer.uvRow0[0] = c * scaleU;
            layer.uvRow0[1] = -s * scaleV;
            layer.uvRow0[2] = transform.mOffset.mV[VX];
            layer.uvRow1[0] = s * scaleU;
            layer.uvRow1[1] = c * scaleV;
            layer.uvRow1[2] = -transform.mOffset.mV[VY];
            std::copy(material.mBaseColor.mV, material.mBaseColor.mV + 4, layer.color);
        }
        const U32 paint = local ? gLocalTerrainMaterials.getPaintType() : composition->getPaintType();
        if (paint == TERRAIN_PAINT_TYPE_PBR_PAINTMAP && !region.toldPaintMap)
        {
            region.toldPaintMap = true;
            LL_INFOS("ASRenderer") << "a region paints its terrain with a paint map, which is not drawn yet: its layers blend by height" << LL_ENDL;
        }
    }
    images[asr::TERRAIN_LAYERS] = ramp.get();

    for (U32 i = 0; i < TERRAIN_IMAGES; ++i)
    {
        if (images[i] != region.images[i])
        {
            if (region.textures[i])
            {
                --region.textures[i]->terrainUsers;
                forgetUnusedTexture(region.textures[i]);
            }
            region.images[i] = images[i];
            region.textures[i] = images[i] ? trackTexture(images[i]) : nullptr;
            if (region.textures[i])
            {
                ++region.textures[i]->terrainUsers;
            }
        }
    }
    for (U32 i = 0; i < asr::TERRAIN_LAYERS; ++i)
    {
        desc.layers[i].texture = textureBits(region.textures[i]);
    }
    const TrackedTexture* rampTexture = region.textures[asr::TERRAIN_LAYERS];
    desc.ramp = asr::uiTextureBits(rampTexture ? rampTexture->slot : 0, asr::UISampler::LinearClamp);

    if (memcmp(&desc, &region.desc, sizeof(desc)) != 0)
    {
        region.desc = desc;
        sScene->setTerrain(region.terrain, desc);
    }
}

// New heights reach the neighbours: the viewer has copied this patch's edges into their
// edge rows (LLSurface::decompressDCTPatch()), and their normals along it read it, but it only
// marks them dirty, with no new update time. A neighbour built from its own data is built
// again once the viewer has regenerated it: a forgotten update time does that. One still
// waiting for its heights is built again now, with the new edge.
void rebuildTerrainNeighbours(LLSurfacePatch* patch)
{
    for (U32 direction = 0; direction < 8; ++direction)
    {
        LLSurfacePatch* neighbour = patch->getNeighborPatch(direction);
        auto region = neighbour ? sTerrainRegions.find(neighbour->getSurface()) : sTerrainRegions.end();
        if (region == sTerrainRegions.end())
        {
            continue;
        }
        const size_t index = size_t(neighbour - neighbour->getSurface()->getPatch(0, 0));
        if (index >= region->second.patches.size())
        {
            continue;
        }
        TerrainPatch& state = region->second.patches[index];
        if (state.real)
        {
            state.key = 0;
        }
        else if (state.busy || finestTerrainGeometry(state.geometries) != 0)
        {
            startTerrainPatch(region->second, neighbour->getSurface(), U32(index));
        }
    }
}

// One look at each patch: a few compares, so every region can be watched every loop.
void watchTerrainRegion(LLViewerRegion* regionp)
{
    LLSurface* surface = &regionp->getLand();
    const S32 perEdge = surface->getPatchesPerEdge();
    if (perEdge <= 0 || !surface->getPatch(0, 0))
    {
        return;
    }
    const auto entry = sTerrainRegions.try_emplace(surface);
    TerrainRegion& region = entry.first->second;
    if (entry.second)
    {
        region.region = regionp;
        region.handle = regionp->getHandle();
        region.agent = surface->getOriginAgent();
        region.spacing = surface->getMetersPerGrid();
        region.perEdge = U32(perEdge);
        region.patches.resize(size_t(perEdge) * perEdge);
        region.terrain = sScene->addTerrain(region.desc);
        boxTerrainRegion(region, surface);
    }
    refreshTerrainLayers(region);

    for (U32 index = 0; index < region.patches.size(); ++index)
    {
        TerrainPatch& state = region.patches[index];
        LLSurfacePatch* patch = surface->getPatch(S32(index % region.perEdge), S32(index / region.perEdge));
        if (terrainPatchReady(patch))
        {
            if (state.real && state.key == patch->getLastUpdateTime())
            {
                continue;
            }
            // Only its own new data reaches its neighbours: a rebuild they asked for does not.
            if (state.seen != patch->getLastUpdateTime())
            {
                state.seen = patch->getLastUpdateTime();
                rebuildTerrainNeighbours(patch);
            }
            startTerrainPatch(region, surface, index);
        }
        else if (!state.real && !state.busy && finestTerrainGeometry(state.geometries) == 0)
        {
            startTerrainPatch(region, surface, index); // flat until its heights come
        }
    }
}

void rewriteTerrainInstances()
{
    for (auto& entry : sTerrainRegions)
    {
        for (TerrainPatch& state : entry.second.patches)
        {
            if (state.instance != NO_INSTANCE)
            {
                writeTerrainInstance(entry.second, state);
            }
        }
    }
}

void updateTerrain()
{
    // An instance's LOD size holds lodScale(), so that the LOD factor cancels out.
    static F32 lastScale = 0.f;
    if (lodScale() != lastScale)
    {
        lastScale = lodScale();
        rewriteTerrainInstances();
    }

    // A region LLWorld no longer lists may be freed already: its entry is all that is used.
    const LLWorld::region_list_t& listed = LLWorld::getInstance()->getRegionList();
    for (auto next = sTerrainRegions.begin(); next != sTerrainRegions.end();)
    {
        const TerrainRegion& region = next->second;
        const bool kept = std::find(listed.begin(), listed.end(), region.region) != listed.end() && &region.region->getLand() == next->first &&
                          region.region->getHandle() == region.handle;
        if (kept)
        {
            ++next;
            continue;
        }
        dropTerrainRegion(next->second);
        next = sTerrainRegions.erase(next);
    }
    for (LLViewerRegion* regionp : listed)
    {
        watchTerrainRegion(regionp);
    }
}

void logObjectFaces(const char* what, LLDrawable* drawable, const WorldObject& object)
{
    LLVOVolume* volume = volumeOf(drawable);
    if (!volume)
    {
        return;
    }
    LL_INFOS("ASRenderer") << what << " " << volume->getID() << " at " << drawable->getPositionAgent() << " size " << volume->getScale() << LL_ENDL;
    for (size_t i = 0; i < object.faces.size(); ++i)
    {
        const ObjectFace& face = object.faces[i];
        const TrackedTexture* texture = face.material ? face.material->texture : nullptr;
        // Include faces with no material/instance: those are precisely what a missing
        // clothing section needs diagnosed, and the old texture-only log omitted them.
        const ShapeFace* shape = object.shape && i < object.shape->faces.size() ? &object.shape->faces[i] : nullptr;
        const SharedSkin* skin = object.bound ? object.bound->skin : nullptr;
        LL_INFOS("ASRenderer") << "  face state " << i << " submitted " << (face.instance != NO_INSTANCE)
                                << " geometry " << (shape ? shape->finest : 0)
                                << " weighted joints by LOD " << (shape ? shape->skinJoints[0] : 0) << "/"
                                << (shape ? shape->skinJoints[1] : 0) << "/" << (shape ? shape->skinJoints[2] : 0) << "/"
                                << (shape ? shape->skinJoints[3] : 0) << " skin joints " << (skin ? skin->joints : 0)
                                << " flags " << face.flags << " alpha " << (face.material ? face.material->desc.color[3] : -1.f)
                                << " cutoff " << (face.material ? face.material->desc.alphaCutoff : -1.f) << LL_ENDL;
        if (texture)
        {
            LLViewerTexture* image = texture->image.get();
            LL_INFOS("ASRenderer") << "  face " << i << " texture " << image->getID() << (image->getType() == LLViewerTexture::MEDIA_TEXTURE ? " media" : "")
                                   << " discard " << image->getDiscardLevel() << " slot " << texture->slot << " slot changes " << texture->changes
                                   << (face.hidden ? " hidden" : "") << " rank " << face.rank << LL_ENDL;
        }
    }
}

// What the shift tool found: the object's faces, and anything else in the same place.
void logShifted()
{
    auto shifted = sObjects.find(sShifted);
    if (shifted == sObjects.end())
    {
        auto leftOut = sLeftOut.find(sShifted);
        LL_INFOS("ASRenderer") << "clicked object is not drawn here: " << (leftOut != sLeftOut.end() ? leftOut->second : "unknown") << LL_ENDL;
        return;
    }

    logObjectFaces("clicked", sShifted, shifted->second);
    constexpr F32 SAME_PLACE = 0.01f; // metres
    const LLVector3 at = sShifted->getPositionAgent();
    U32 twins = 0;
    for (const auto& entry : sObjects)
    {
        if (entry.first != sShifted && (entry.first->getPositionAgent() - at).length() < SAME_PLACE && ++twins <= 10)
        {
            logObjectFaces("same place", entry.first, entry.second);
        }
    }
    LL_INFOS("ASRenderer") << twins << " other objects in the same place" << LL_ENDL;
}

void updateLocalShift()
{
    static LLCachedControl<F32> shift(gSavedSettings, "ASRendererLocalShift", 0.f);
    if (shift() == sShift)
    {
        return;
    }

    if (sShifted)
    {
        sMoved.insert(sShifted); // puts it back
    }
    sShift = shift();
    sShifted = nullptr;
    LLPointer<LLViewerObject> object = sShift != 0.f ? LLToolPie::getInstance()->getPick().getObject() : LLPointer<LLViewerObject>();
    if (object.notNull() && object->mDrawable.notNull())
    {
        sShifted = object->mDrawable.get();
        sMoved.insert(sShifted);
        logShifted();
    }
}

void publishCamera()
{
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 eye = camera->getOrigin() + sRenderOrigin;
    asr::SceneCamera3D scene = asr::makeCameraFromAxes(eye.mV, camera->getAtAxis().mV, camera->getUpAxis().mV, camera->getView(),
                                                       camera->getAspect(), camera->getNear(), camera->getFar());

    scene.lodScale = lodScale();
    static LLCachedControl<U32> debugView(gSavedSettings, "ASRendererDebugView", 0);
    scene.debugView = debugView();
    asr::CameraTurn turn;
    ASRCameraTurn::publish(turn, sRenderOrigin);
    ASRViewer::setCamera(scene, turn);
}

// Most of a region's objects sit in the object cache, and only become objects once the
// cache finds them in view. The viewer does that in updateCull(), which does not run here.
void cullObjectCache()
{
    LLDrawable::incrementVisible();
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        LLVOCachePartition* partition = region->getVOCachePartition();
        if (partition)
        {
            partition->cull(*camera, false);
        }
    }

    // LLVOAvatar acts on visibility, which the viewer sets in updateCull(). A hidden avatar
    // never finishes loading, and the viewer then holds back every detach. Self always
    // counts, wherever the camera looks, and the others count as they do in OpenGL.
    constexpr F32 AVATAR_RADIUS = 4.f; // metres: a body, and what it usually wears
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
        if (avatar->mDrawable.notNull() && !avatar->isDead()
            && (avatar->isSelf() || camera->sphereInFrustum(avatar->getRenderPosition(), AVATAR_RADIUS)))
        {
            avatar->mDrawable->setVisible(*camera);
        }
    }
}

// Temporary: what update() and the whole main thread loop spend, since the last log.
enum UpdatePart { PART_MESH, PART_CACHE, PART_SHAPES, PART_TEXTURES, PART_LOD, PART_SWEEP, PART_AVATARS, PART_APPLY, PART_MOVE, PART_TERRAIN, PART_COUNT };
constexpr const char* PART_NAMES[PART_COUNT] = { "mesh repository", "object cache", "finished shapes", "follow textures", "LOD queue", "sweep", "avatars", "apply", "move", "terrain" };
F64 sPartSeconds[PART_COUNT] = {};
F64 sPartWorst[PART_COUNT] = {};
constexpr const char* MAIN_PART_NAMES[ASRWorld::MAIN_PART_COUNT] = { "textures", "camera", "create objects", "update geometry", "UI",
                                                                                 "network", "idle callbacks", "window update", "object list", "cleanup", "move", "particles", "apparent angles",
                                                                                 "input and coroutines", "idle() as a whole, counted again", "snapshots", "texture threads", "mesh update",
                                                                                 "display() as a whole, counted again", "sleep and threads at the end, counted again",
                                                                                 "idle() start and agent update", "agent position", "effects and regions", "camera and media", "audio" };
F64 sMainSeconds[ASRWorld::MAIN_PART_COUNT] = {};
F64 sMainWorst[ASRWorld::MAIN_PART_COUNT] = {};
F64 sWorstLoop = 0.0;
F64 sLastLogSeconds = 0.0;
U32 sLoops = 0;
std::chrono::steady_clock::time_point sLoopStart;

template <typename Work>
void timed(UpdatePart part, Work work)
{
    const auto start = std::chrono::steady_clock::now();
    work();
    const F64 seconds = std::chrono::duration<F64>(std::chrono::steady_clock::now() - start).count();
    sPartSeconds[part] += seconds;
    sPartWorst[part] = llmax(sPartWorst[part], seconds);
}

void logCounts()
{
    const auto now = std::chrono::steady_clock::now();
    if (sLoops != 0)
    {
        sWorstLoop = llmax(sWorstLoop, std::chrono::duration<F64>(now - sLoopStart).count());
    }
    sLoopStart = now;
    ++sLoops;

    if (now - sLogged >= LOG_PERIOD)
    {
        const F64 elapsed = std::chrono::duration<F64>(now - sLogged).count();
        sLogged = now;
        std::ostringstream parts;
        F64 accounted = 0.0;
        for (U32 i = 0; i < PART_COUNT; ++i)
        {
            parts << ", " << PART_NAMES[i] << " " << sPartSeconds[i] * 1000.0 / llmax(sLoops, 1u) << "/" << sPartWorst[i] * 1000.0;
            accounted += sPartSeconds[i];
            sPartSeconds[i] = 0.0;
            sPartWorst[i] = 0.0;
        }
        std::ostringstream mainParts;
        for (U32 i = 0; i < ASRWorld::MAIN_PART_COUNT; ++i)
        {
            mainParts << ", " << MAIN_PART_NAMES[i] << " " << sMainSeconds[i] * 1000.0 / llmax(sLoops, 1u) << "/" << sMainWorst[i] * 1000.0;
            // The wholes hold parts counted already.
            const bool whole = i == ASRWorld::MAIN_IDLE_WHOLE || i == ASRWorld::MAIN_DISPLAY_WHOLE || i == ASRWorld::MAIN_SLEEP_WHOLE;
            accounted += whole ? 0.0 : sMainSeconds[i];
            sMainSeconds[i] = 0.0;
            sMainWorst[i] = 0.0;
        }
        LL_INFOS("ASRenderer") << "main thread: " << sLoops / elapsed << " loops a second, worst loop " << sWorstLoop * 1000.0 << " ms, last log took " << sLastLogSeconds * 1000.0
                               << " ms; update() average/worst ms" << parts.str() << "; display() and idle() average/worst ms" << mainParts.str() << "; everything else, average ms "
                               << (elapsed - accounted) * 1000.0 / llmax(sLoops, 1u) << LL_ENDL;
        sLoops = 0;
        sWorstLoop = 0.0;

        const asr::SceneStats stats = ASRViewer::device()->sceneStats();
        std::map<std::string, U32> leftOut;
        for (const auto& object : sLeftOut)
        {
            ++leftOut[object.second];
        }
        // Where the scene is against the camera, to tell a misplaced world from a hidden one.
        const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
        F32 nearest = F32_MAX;
        LLVector3 nearestAt;
        LLVector3 nearestRegion;
        U32 active = 0;
        for (const auto& entry : sObjects)
        {
            LLVOVolume* volume = volumeOf(entry.first);
            if (volume && entry.second.shape && entry.second.shape->ready)
            {
                asr::InstanceDesc desc;
                fillTransform(desc, entry.first, volume, *entry.second.shape);
                const LLVector3 at = LLVector3(desc.lodCenter) - sRenderOrigin;
                active += entry.first->isActive() ? 1 : 0;
                if ((at - eye).length() < nearest)
                {
                    nearest = (at - eye).length();
                    nearestAt = at;
                    nearestRegion = entry.first->getRegion() ? entry.first->getRegion()->getOriginAgent() : LLVector3();
                }
            }
        }
        LL_INFOS("ASRenderer") << "nearest object " << nearest << " m away, at " << nearestAt << ", its region at " << nearestRegion << ", " << active
                               << " active, looking along " << LLViewerCamera::getInstance()->getAtAxis() << ", far " << LLViewerCamera::getInstance()->getFar()
                               << ", near " << LLViewerCamera::getInstance()->getNear() << ", view " << LLViewerCamera::getInstance()->getView() << ", aspect "
                               << LLViewerCamera::getInstance()->getAspect() << LL_ENDL;
        U32 rigged = 0;
        U32 attached = 0;
        for (const auto& entry : sObjects)
        {
            if (entry.second.bound)
            {
                ++(entry.second.bound->skin ? rigged : attached);
            }
        }
        // How far the farthest joint is from the first: the skinned vertices' box grows with it.
        F32 spread = 0.f;
        U32 parked = 0;
        for (const auto& entry : sSkeletons)
        {
            parked += entry.second.resident ? 0 : 1;
            const std::vector<F32>& rows = entry.second.rows;
            for (size_t at = 12; at + 12 <= rows.size(); at += 12)
            {
                const LLVector3 offset(rows[at + 3] - rows[3], rows[at + 7] - rows[7], rows[at + 11] - rows[11]);
                spread = llmax(spread, offset.length());
            }
        }
        LL_INFOS("ASRenderer") << "avatars: " << sSkeletons.size() << " skeletons (" << parked << " parked out of view), " << sSkins.size() << " skins, " << rigged << " rigged and "
                               << attached << " attached objects, " << sAwaitingAvatar.size() << " waiting for their avatar, farthest joint "
                               << spread << " m from the root, avatar residency pressure " << sAvatarResidency.underPressure()
                               << ", full-detail avatars " << sAvatarDetail.fullDetail() << ", sharp within " << sNearMetres << " m" << LL_ENDL;
        const ASRAvatarBody::Counts body = ASRAvatarBody::takeCounts();
        LL_INFOS("ASRenderer") << "system bodies: " << body.bodies << " bodies, " << body.parts << " parts shown; since the last log "
                               << body.weightUpdates << " morph weight updates, " << body.maskUpdates << " masks, " << body.lodChanges
                               << " LOD changes" << LL_ENDL;

        U32 unloaded = 0;
        LLViewerTexture* sample = nullptr;
        for (const auto& texture : sTextures)
        {
            if (texture.second.slot == 0)
            {
                ++unloaded;
                sample = texture.second.image.get();
            }
        }
        LL_INFOS("ASRenderer") << unloaded << " of " << sTextures.size() << " textures have no image, discard bias " << LLViewerTexture::sDesiredDiscardBias
                               << ", texture MB " << LLImageGL::getTextureBytesAllocated() / (1024 * 1024) << ", VRAM MB " << gGLManager.mVRAM;
        if (sample)
        {
            LL_CONT << ", for example " << sample->getID() << " discard " << sample->getDiscardLevel() << " size " << sample->getMaxVirtualSize()
                    << " faces " << sample->getTotalNumFaces() << " name " << sample->getTexName();
        }
        LL_CONT << LL_ENDL;

        // Temporary: what the textures with no image are waiting for.
        U32 missing = 0;
        U32 fetching = 0;
        U32 idle = 0;
        U32 idleWornInView = 0;
        U32 idleWornOutOfView = 0;
        U32 idleNotWorn = 0;
        F32 idleNearest = F32_MAX;
        LLViewerFetchedTexture* idleNearestImage = nullptr;
        std::vector<std::pair<F32, LLViewerFetchedTexture*>> waiting;
        for (const auto& texture : sTextures)
        {
            LLViewerFetchedTexture* fetched = texture.second.slot == 0 ? LLViewerTextureManager::staticCastToFetchedTexture(texture.second.image.get()) : nullptr;
            if (fetched)
            {
                missing += fetched->isMissingAsset() ? 1 : 0;
                fetching += fetched->isFetching() ? 1 : 0;
                idle += !fetched->isMissingAsset() && !fetched->isFetching() ? 1 : 0;
                waiting.emplace_back(fetched->getMaxVirtualSize(), fetched);
                // Temporary: idle images never requested, by where their wearer is, with
                // judgeTexture()'s own test. Gray meshes on avatars in view land in the first.
                if (!fetched->isMissingAsset() && !fetched->isFetching())
                {
                    bool worn = false;
                    bool wornInView = false;
                    F32 nearest = F32_MAX;
                    for (const auto& user : texture.second.users)
                    {
                        const auto object = sObjects.find(user.first);
                        LLVOAvatar* avatar = object != sObjects.end() && object->second.bound ? object->second.bound->avatar : nullptr;
                        if (!avatar || avatar->isDead()) continue;
                        worn = true;
                        const LLVector3 center = avatar->getRenderPosition();
                        const LLVector3* extents = avatar->getLastAnimExtents();
                        const F32 radius = llmax((extents[1] - extents[0]).length() * 0.5f, 1.5f);
                        if (LLViewerCamera::getInstance()->sphereInFrustum(center, radius))
                        {
                            wornInView = true;
                            nearest = llmin(nearest, (center - LLViewerCamera::getInstance()->getOrigin()).length());
                        }
                    }
                    if (wornInView)
                    {
                        ++idleWornInView;
                        if (nearest < idleNearest)
                        {
                            idleNearest = nearest;
                            idleNearestImage = fetched;
                        }
                    }
                    idleWornOutOfView += worn && !wornInView ? 1 : 0;
                    idleNotWorn += !worn ? 1 : 0;
                }
            }
        }
        const size_t shown = llmin(waiting.size(), size_t(5));
        std::partial_sort(waiting.begin(), waiting.begin() + shown, waiting.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        LL_INFOS("ASRenderer") << "no image: " << missing << " missing, " << fetching << " fetching, " << idle << " idle" << LL_ENDL;
        LL_INFOS("ASRenderer") << "idle with no image: " << idleWornInView << " worn by avatars in view, " << idleWornOutOfView << " worn out of view, "
                               << idleNotWorn << " not worn" << LL_ENDL;
        if (idleNearestImage)
        {
            LL_INFOS("ASRenderer") << "  nearest in view " << idleNearestImage->getID() << " at " << idleNearest << " m, size " << idleNearestImage->getMaxVirtualSize()
                                   << " desired discard " << idleNearestImage->getDesiredDiscardLevel() << " max discard " << idleNearestImage->getMaxDiscardLevel()
                                   << " boost " << idleNearestImage->getBoostLevel() << " faces " << idleNearestImage->getTotalNumFaces() << LL_ENDL;
        }
        const asr::MemoryBudget& memory = ASRViewer::device()->memoryBudget();
        LL_INFOS("ASRenderer") << "texture memory " << memory.used(asr::MemoryCategory::Textures) / (1024 * 1024) << " of "
                               << memory.textureLimit() / (1024 * 1024) << " MB; all ours " << memory.totalUsed() / (1024 * 1024) << " of "
                               << memory.budget() / (1024 * 1024) << " MB; driver says the process uses "
                               << ASRViewer::device()->driverUsageBytes / (1024 * 1024) << " of " << ASRViewer::device()->driverBudgetBytes / (1024 * 1024)
                               << " MB; allocator holds " << ASRViewer::device()->allocatorBlockBytes / (1024 * 1024) << " MB, "
                               << ASRViewer::device()->allocatorUsedBytes / (1024 * 1024) << " MB of it in use" << LL_ENDL;

        // Temporary: live must match the memory above, or something leaks. Drawn says how
        // much of it faces we actually draw are using.
        U32 live = 0;
        U64 liveBytes = 0;
        U32 shrunk = 0;
        ASR::getLiveTextures(live, liveBytes, shrunk);
        U64 drawnBytes = 0;
        for (const auto& texture : sTextures)
        {
            if (texture.second.slot != 0)
            {
                drawnBytes += U64(texture.second.image->getWidth()) * texture.second.image->getHeight() * 4 * 4 / 3;
            }
        }
        LL_INFOS("ASRenderer") << "live textures " << live << ", " << liveBytes / (1024 * 1024) << " MB, on drawn faces about " << drawnBytes / (1024 * 1024)
                               << " MB, shrunk so far " << shrunk << ", frozen " << LLViewerTexture::sFreezeImageUpdates << ", free slots " << ASR::freeTextureSlots()
                               << ", unseen limit " << (sUnseenLimit < F32_MAX ? sUnseenLimit : -1.f) << " s, released so far " << sReleasedTextures
                               << ", lost so far " << sLostTextures << LL_ENDL;
        U32 seenNow = 0;
        U32 levels[asr::FEEDBACK_MAX_LEVEL + 1] = {};
        for (const auto& tracked : sTextures)
        {
            if (gFrameTimeSeconds - tracked.second.gpuSeenAt < 0.5f)
            {
                ++seenNow;
                ++levels[tracked.second.gpuLevel];
            }
        }
        std::ostringstream levelCounts;
        for (U32 level = 0; level <= asr::FEEDBACK_MAX_LEVEL; ++level)
        {
            if (levels[level] != 0)
            {
                levelCounts << " " << (1u << level) << ":" << levels[level];
            }
        }
        LL_INFOS("ASRenderer") << "texture feedback: " << sFeedbackWindows << " windows since the last log, " << seenNow << " of " << sTextures.size()
                               << " drawn textures seen now, judgements sized by it " << sFeedbackSized << ", hidden " << sFeedbackHidden
                               << "; feedback age " << (sLastFeedback > -1000.f ? gFrameTimeSeconds.value() - sLastFeedback : -1.f) << " s; mip copies rebased/rejected "
                               << stats.textureCopiesRebased << "/" << stats.textureCopiesRejected
                               << "; texels across needed, by count:" << levelCounts.str() << LL_ENDL;
        sFeedbackWindows = 0;
        sFeedbackSized = 0;
        sFeedbackHidden = 0;
        for (size_t i = 0; i < shown; ++i)
        {
            LLViewerFetchedTexture* fetched = waiting[i].second;
            LL_INFOS("ASRenderer") << "  waiting " << fetched->getID() << " size " << waiting[i].first << " desired discard " << fetched->getDesiredDiscardLevel()
                                   << " max discard " << fetched->getMaxDiscardLevel() << " fetching " << fetched->isFetching() << " missing " << fetched->isMissingAsset()
                                   << " fetcher " << fetched->hasFetcher() << " boost " << fetched->getBoostLevel() << " faces " << fetched->getTotalNumFaces() << LL_ENDL;
        }

        F32 whiteAngle = 0.f; // the white face that fills the most of the view
        LLDrawable* whiteObject = nullptr;
        const TrackedTexture* whiteTexture = nullptr;
        U32 facesWhite = 0;
        U32 facesHidden = 0;
        U32 facesNoTexture = 0;
        for (const auto& object : sObjects)
        {
            for (const ObjectFace& face : object.second.faces)
            {
                if (face.instance != NO_INSTANCE)
                {
                    const TrackedTexture* texture = face.material->texture;
                    facesNoTexture += texture ? 0 : 1;
                    facesWhite += !texture || texture->slot == 0 ? 1 : 0;
                    facesHidden += face.hidden ? 1 : 0;

                    LLVOVolume* volume = volumeOf(object.first);
                    if (texture && texture->slot == 0 && !face.hidden && volume)
                    {
                        const F32 angle = volume->getScale().length() / llmax((object.first->getPositionAgent() - eye).length(), 1.f);
                        if (angle > whiteAngle)
                        {
                            whiteAngle = angle;
                            whiteObject = object.first;
                            whiteTexture = texture;
                        }
                    }
                }
            }
        }
        if (whiteObject)
        {
            LL_INFOS("ASRenderer") << "largest white object " << whiteObject->getVObj()->getID() << " at " << whiteObject->getPositionAgent() << " size "
                                   << volumeOf(whiteObject)->getScale() << ", " << (whiteObject->getPositionAgent() - eye).length() << " m away, texture "
                                   << whiteTexture->image->getID() << " size " << whiteTexture->image.get()->getMaxVirtualSize() << LL_ENDL;
        }
        U32 outOfDate = 0; // should stay 0: it proves no change goes unreported
        for (const auto& texture : sTextures)
        {
            outOfDate += texture.second.slot != ASR::textureSlot(texture.second.image->getTexName()) ? 1 : 0;
        }
        U32 created = 0;
        U32 blank = 0;
        U32 filled = 0;
        U32 subImages = 0;
        ASR::getTextureCounts(created, blank, filled, subImages);
        LL_INFOS("ASRenderer") << "faces on white " << facesWhite << ", hidden by a copy " << facesHidden << ", with no texture " << facesNoTexture << ", textures out of date " << outOfDate
                               << "; textures created " << created << ", created blank " << blank << ", filled " << filled << ", sub images " << subImages
                               << ", lost their image " << sLostTextures << LL_ENDL;
        sLostTextures = 0;
        if (isAgentAvatarValid())
        {
            LL_INFOS("ASRenderer") << "self fully loaded " << gAgentAvatarp->isFullyLoaded() << ", rezzed status " << gAgentAvatarp->getRezzedStatus()
                                   << ", visible " << gAgentAvatarp->isVisible() << ", meshes pending " << gAgentAvatarp->hasPendingAttachedMeshes() << LL_ENDL;
        }
        if (sShifted)
        {
            logShifted();
        }

        // Temporary: how mesh is coming in, and how much repeating saves.
        U32 meshes = 0;
        U32 meshesWaiting = 0;
        U32 meshLods[LODS + 1] = {};
        U32 lodsHeld = 0;
        size_t meshUsers = 0;
        for (const auto& shape : sShapes)
        {
            if (isMeshKey(shape.first))
            {
                ++meshes;
                meshesWaiting += shape.second->ready ? 0 : 1;
                ++meshLods[shape.second->nextLod];
                meshUsers += shape.second->users.size();
                for (const LLPointer<LLVolume>& arrived : shape.second->arrived)
                {
                    lodsHeld += arrived.notNull() ? 1 : 0;
                }
            }
        }
        const asr::MemoryBudget& budget = ASRViewer::device()->memoryBudget();
        LL_INFOS("ASRenderer") << meshes << " meshes shown by " << meshUsers << " objects, " << meshesWaiting << " with nothing yet, built up to LOD 0/1/2/3: " << meshLods[1] << "/"
                               << meshLods[2] << "/" << meshLods[3] << "/" << meshLods[4] << ", " << lodsHeld << " LODs waiting for a coarser one, " << sChanged.size()
                               << " objects left to apply, geometry memory " << budget.used(asr::MemoryCategory::Geometry) / (1024 * 1024) << " of "
                               << budget.limit(asr::MemoryCategory::Geometry) / (1024 * 1024) << " MB" << (memoryPressure() ? ", memory short" : "")
                               << LL_ENDL;

        // Temporary: what fills texture memory, and whether anything still wants to change.
        constexpr U32 DISCARDS = 7;
        U32 atDiscard[DISCARDS] = {};
        U64 bytesAtDiscard[DISCARDS] = {};
        U32 wantSharper = 0;
        U32 wantSmaller = 0;
        U32 drawn = 0;
        U32 notDrawn[4] = {};      // diffuse, normal map, specular map, no face at all
        U64 notDrawnBytes[4] = {};
        U32 sharpest = 0;          // drawn at discard 0, and how many of those were seen lately
        U32 sharpestSeen = 0;
        U64 sharpestBytes = 0;
        U64 sharpestSeenBytes = 0;
        for (auto& image : gTextureList)
        {
            if (!image->hasGLTexture())
            {
                continue;
            }
            const U64 bytes = U64(image->getWidth()) * image->getHeight() * 4 * 4 / 3;
            const U32 discard = U32(llclamp(image->getDiscardLevel(), 0, S32(DISCARDS) - 1));
            ++atDiscard[discard];
            bytesAtDiscard[discard] += bytes;
            const S32 desired = const_cast<LLViewerFetchedTexture*>(image.get())->getDesiredDiscardLevel(); // not a const function
            wantSharper += desired < image->getDiscardLevel() ? 1 : 0;
            wantSmaller += desired > image->getDiscardLevel() ? 1 : 0;
            if (sTextures.count(image->getGLTexture()) != 0)
            {
                ++drawn;
                if (discard == 0)
                {
                    constexpr F32 LATELY = 10.f;
                    const bool seen = gFrameTimeSeconds - image->mLastSeen < LATELY;
                    ++sharpest;
                    sharpestBytes += bytes;
                    sharpestSeen += seen ? 1 : 0;
                    sharpestSeenBytes += seen ? bytes : 0;
                }
                continue;
            }
            const U32 use = image->getNumFaces(LLRender::DIFFUSE_MAP) > 0 ? 0 : image->getNumFaces(LLRender::NORMAL_MAP) > 0 ? 1 : image->getNumFaces(LLRender::SPECULAR_MAP) > 0 ? 2 : 3;
            ++notDrawn[use];
            notDrawnBytes[use] += bytes;
        }
        std::ostringstream discards;
        for (U32 i = 0; i < DISCARDS; ++i)
        {
            discards << " " << i << ":" << atDiscard[i] << "/" << bytesAtDiscard[i] / (1024 * 1024) << "MB";
        }
        LL_INFOS("ASRenderer") << "by discard" << discards.str() << ", want sharper " << wantSharper << ", want smaller " << wantSmaller << "; on drawn faces " << drawn
                               << ", not drawn: diffuse " << notDrawn[0] << "/" << notDrawnBytes[0] / (1024 * 1024) << "MB, normal " << notDrawn[1] << "/" << notDrawnBytes[1] / (1024 * 1024)
                               << "MB, specular " << notDrawn[2] << "/" << notDrawnBytes[2] / (1024 * 1024) << "MB, no face " << notDrawn[3] << "/" << notDrawnBytes[3] / (1024 * 1024)
                               << "MB; drawn at discard 0: " << sharpest << "/" << sharpestBytes / (1024 * 1024) << "MB, in view lately " << sharpestSeen << "/"
                               << sharpestSeenBytes / (1024 * 1024) << "MB" << LL_ENDL;

        // Temporary: where a frame's GPU time goes.
        std::ostringstream timings;
        double gpuTotal = 0.0;
        for (U32 i = 0; i < stats.passCount; ++i)
        {
            timings << ", " << stats.passes[i].name << " " << stats.passes[i].milliseconds;
            gpuTotal += stats.passes[i].milliseconds;
        }
        LL_INFOS("ASRenderer") << "GPU ms " << gpuTotal << timings.str() << ", retest entries " << stats.retestEntries << ", groups " << stats.groupEntries
                               << ", chunks " << stats.chunks << ", skin work run/in use: palette " << stats.paletteEnd << "/" << stats.paletteLive << ", vertex blocks "
                               << stats.vertexBlockEnd << "/" << stats.vertexBlockLive << ", cluster blocks " << stats.clusterBlockEnd << "/"
                               << stats.clusterBlockLive << LL_ENDL;
        constexpr U64 MB = 1024 * 1024;
        const U64 geometry = ASRViewer::device()->memoryBudget().used(asr::MemoryCategory::Geometry);
        const U64 known = stats.chunkBytes + stats.tableBytes;
        LL_INFOS("ASRenderer") << "geometry MB: chunks " << stats.chunkBytes / MB << " (" << stats.chunkFreeBytes / MB << " free), tables "
                               << stats.tableBytes / MB << " (deformed positions " << stats.skinnedPositionBytes / MB << ", clusters "
                               << stats.clusterTableBytes / MB << "), lists and the rest " << (geometry > known ? geometry - known : 0) / MB << LL_ENDL;
        if (stats.guardPixels != 0)
        {
            // Temporary: GUARD_* of asrscene.h, the resolve's checks that stopped a bad read.
            LL_WARNS("ASRenderer") << "resolve guards since the start: bits " << stats.guardBits << " (1 entry, 2 instance, 4 cluster, 8 material, 16 addresses), "
                                   << stats.guardPixels << " pixels, largest bad value " << stats.guardValue << LL_ENDL;
        }
        if (stats.droppedEntries != 0)
        {
            LL_WARNS("ASRenderer") << "full cull lists turned away " << stats.droppedEntries << " entries since the start" << LL_ENDL;
        }
        size_t terrainPatches = 0;
        U32 terrainDrawn = 0;
        U32 terrainReal = 0;
        U32 terrainBusy = 0;
        for (const auto& region : sTerrainRegions)
        {
            terrainPatches += region.second.patches.size();
            for (const TerrainPatch& patch : region.second.patches)
            {
                terrainDrawn += patch.instance != NO_INSTANCE ? 1 : 0;
                terrainReal += patch.instance != NO_INSTANCE && patch.real ? 1 : 0;
                terrainBusy += patch.busy ? 1 : 0;
            }
        }
        LL_INFOS("ASRenderer") << "terrain: " << sTerrainRegions.size() << " regions, " << terrainDrawn << " of " << terrainPatches << " patches drawn, "
                               << terrainDrawn - terrainReal << " of them flat while their heights come, " << terrainBusy << " building, " << sTerrainBuilt
                               << " built and " << sTerrainReboxed << " regions reboxed since the last log" << LL_ENDL;
        sTerrainBuilt = 0;
        sTerrainReboxed = 0;

        std::ostringstream reasons;
        for (const auto& reason : leftOut)
        {
            reasons << ", " << reason.second << " " << reason.first;
        }
        LL_INFOS("ASRenderer") << sObjects.size() << " objects, " << sShapes.size() << " shapes, " << sMaterials.size() << " materials, " << sTextures.size() << " textures, "
                               << stats.instances << " instances, " << stats.triangles << " triangles, " << stats.earlyClusters << " + " << stats.lateClusters
                               << " clusters drawn, " << stats.pendingGeometries << " geometries waiting, camera at " << LLViewerCamera::getInstance()->getOrigin()
                               << ", left out:" << reasons.str() << LL_ENDL;

        // This log walks everything, so its own cost must not be read as a viewer stall.
        sLastLogSeconds = std::chrono::duration<F64>(std::chrono::steady_clock::now() - now).count();
        sLoopStart = std::chrono::steady_clock::now();
    }
}

} // namespace

void ASRWorld::start()
{
    sScene = &ASRViewer::device()->scene();
    sRunning = true;
    sStopping = false;
    sLogged = std::chrono::steady_clock::now();
    ASR::watchImages(true);
    const U32 workers = llclamp(std::thread::hardware_concurrency() / 4, 1u, MAX_WORKERS);
    for (U32 i = 0; i < workers; ++i)
    {
        sWorkers.emplace_back(workerLoop);
    }
}

void ASRWorld::stop()
{
    if (!sRunning)
    {
        return;
    }
    sRunning = false;
    ASR::watchImages(false);
    {
        std::lock_guard<std::mutex> lock(sJobMutex);
        sStopping = true;
    }
    sJobReady.notify_all();
    for (std::thread& worker : sWorkers)
    {
        worker.join();
    }
    sWorkers.clear();
    sJobs.clear();
    sFinished.clear();
    sChanged.clear();
    sMoved.clear();
    sShifted = nullptr;
    sStacks.clear();
    sSweep.clear();
    sSweepNext = 0;
    sLodChecks = {};
    sCameraTravel = 0.0;
    sObjects.clear();
    sAwaitingAvatar.clear();
    sSkeletons.clear();
    sAvatarResidency = {};
    sBodyAvatars.clear();
    ASRAvatarBody::forget();
    sSkins.clear();
    sLeftOut.clear();
    sTerrainRegions.clear();
    sMaterials.clear();
    sSlotOwners.clear();
    sTextures.clear();
    sShapes.clear();
    sScene = nullptr;
}

void ASRWorld::objectChanged(LLDrawable* drawable)
{
    if (sRunning && volumeOf(drawable))
    {
        sChanged.insert(drawable);
    }
}

void ASRWorld::objectMoved(LLDrawable* drawable)
{
    if (sRunning && volumeOf(drawable))
    {
        markMoved(drawable->getVObj().get());
    }
}

void ASRWorld::avatarMasksChanged(LLVOAvatar* avatar)
{
    if (sRunning)
    {
        ASRAvatarBody::masksChanged(avatar);
    }
}

void ASRWorld::avatarAnimated(LLVOAvatar* avatar)
{
    if (sRunning)
    {
        const auto found = sSkeletons.find(avatar);
        if (found != sSkeletons.end())
        {
            found->second.animated = true;
        }
    }
}

S32 ASRWorld::animationPeriod(LLVOAvatar* avatar)
{
    if (!sRunning || avatar->isSelf())
    {
        return 0;
    }
    // No impostors are drawn, so the viewer's impostor periods would freeze avatars drawn
    // in full. Ours come with the GPU blending between poses: see setAnimationPeriods().
    const auto found = sSkeletons.find(avatar);
    return found != sSkeletons.end() ? found->second.animationPeriod : 0;
}

// For asravatarbody.cpp: see asrworldshared.h.
asr::SceneQueue& ASRWorldShared::scene()
{
    return *sScene;
}

ASRSharedMaterial* ASRWorldShared::useMaterial(const asr::MaterialDesc& desc, LLViewerTexture* image, LLDrawable* user, bool eye)
{
    return ::useMaterial(desc, image, user, eye);
}

void ASRWorldShared::releaseMaterial(ASRSharedMaterial* material, LLDrawable* user, bool eye)
{
    ::releaseMaterial(material, user, eye);
}

U32 ASRWorldShared::materialId(const ASRSharedMaterial* material)
{
    return material->id;
}

void ASRWorldShared::visitMaterialTexture(ASRSharedMaterial* material)
{
    TrackedTexture* texture = material->texture;
    if (texture && gFrameTimeSeconds - texture->visited >= LOD_SWEEP_SECONDS)
    {
        texture->visited = gFrameTimeSeconds;
        visitTexture(*texture);
    }
}

void ASRWorldShared::rowsOf(const LLMatrix4& matrix, F32* rows)
{
    ::rowsOf(matrix, rows);
}

void ASRWorld::objectRemoved(LLDrawable* drawable)
{
    if (!sRunning)
    {
        return;
    }
    sChanged.erase(drawable);
    sMoved.erase(drawable);
    sLeftOut.erase(drawable);
    sSelected.erase(drawable);
    sHudObjects.erase(drawable);
    if (drawable == sShifted)
    {
        sShifted = nullptr;
    }
    dropObject(drawable);
}

void ASRWorld::updateTerrainGL()
{
    if (!sRunning)
    {
        return;
    }
    std::list<LLGLUpdate*>& queue = LLGLUpdate::sGLQ;
    for (auto next = queue.begin(); next != queue.end();)
    {
        LLVOSurfacePatch* patch = dynamic_cast<LLVOSurfacePatch*>(*next);
        if (!patch)
        {
            ++next;
            continue;
        }
        next = queue.erase(next);
        patch->mInQ = false;
        patch->updateGL();
    }
}

void ASRWorld::addMainTime(MainPart part, F32 seconds)
{
    sMainSeconds[part] += seconds;
    sMainWorst[part] = llmax(sMainWorst[part], F64(seconds));
}

void ASRWorld::meshLoaded(const LLVolumeParams& params, LLVolume* volume, S32 lod)
{
    if (!sRunning || !volume)
    {
        return;
    }

    // With no shape nobody is waiting: a later object asks again, and that reads the disk.
    auto found = sShapes.find(meshKey(params));
    if (found != sShapes.end() && U32(lod) >= found->second->nextLod)
    {
        found->second->arrived[lod] = volume;
        startNextMeshLod(*found->second);
    }
}

void ASRWorld::meshUnavailable(const LLVolumeParams& params, S32 lod)
{
    if (!sRunning)
    {
        return;
    }

    auto found = sShapes.find(meshKey(params));
    if (found != sShapes.end())
    {
        found->second->gone[lod] = true;
        startNextMeshLod(*found->second);
    }
}

void ASRWorld::worldShifted(const LLVector3& offset)
{
    if (!sRunning)
    {
        return;
    }

    // Floats lose metres of range to precision, so render space goes back to agent space when
    // that costs nothing: a teleport, whose screen hides it and whose old objects are going
    // anyway. 8 km is 32 regions on foot with no teleport, and a position there still has
    // half a millimetre of precision.
    constexpr F32 LARGEST_ORIGIN = 8192.f;
    sRenderOrigin -= offset;
    for (auto& region : sTerrainRegions)
    {
        region.second.agent += offset;
    }
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || sRenderOrigin.length() > LARGEST_ORIGIN)
    {
        sRenderOrigin.clear();
        for (const auto& object : sObjects)
        {
            sMoved.insert(object.first);
        }
        rewriteTerrainInstances();
    }
}

S32 ASRWorld::avatarDetailDiscard(LLViewerFetchedTexture* image)
{
    static LLCachedControl<bool> fullRes(gSavedSettings, "TextureLoadFullRes", false);
    if (!sRunning || fullRes() || !image->getUseMipMaps() || image->getType() != LLViewerTexture::LOD_TEXTURE ||
        (image->getBoostLevel() != LLViewerTexture::BOOST_NONE && image->getBoostLevel() != LLViewerTexture::BOOST_AVATAR_BAKED))
    {
        return 0;
    }
    const auto found = sTextures.find(image->getGLTexture());
    if (found == sTextures.end() || found->second.terrainUsers || found->second.users.empty()) return 0;

    TrackedTexture& texture = found->second;
    const LLVector3 camera = LLViewerCamera::getInstance()->getOrigin();
    U32 limit = 0;
    for (const auto& [drawable, uses] : texture.users)
    {
        if (!drawable || drawable->isDead()) return 0;
        F32 distance;
        if (uses.eyes == uses.faces)
        {
            LLViewerObject* object = drawable->getVObj().get();
            if (!object || !object->isAvatar()) return 0;
            LLJoint* eye = static_cast<LLVOAvatar*>(object)->getJoint("mEyeLeft");
            if (!eye) return 0;
            distance = (eye->getWorldPosition() - camera).length();
        }
        else
        {
            const auto object = sObjects.find(drawable);
            LLVOVolume* volume = volumeOf(drawable);
            if (object == sObjects.end() || !object->second.bound || !volume) return 0;
            const LLVector3& size = volume->getScale();
            constexpr F32 SMALL_ATTACHMENT_METRES = 0.25f;
            if (llmax(size.mV[0], size.mV[1], size.mV[2]) > SMALL_ATTACHMENT_METRES) return 0;

            const AvatarBinding& bound = *object->second.bound;
            if (bound.skin)
            {
                // Rigged attachments have no reliable CPU position of their own. The
                // distance to the wearer's animated bounds favors detail when in doubt.
                if (!bound.avatar || bound.avatar->isDead()) return 0;
                const LLVector3* box = bound.avatar->getLastAnimExtents();
                if (box[0] == box[1]) return 0;
                LLVector3 nearest;
                for (U32 axis = 0; axis < 3; ++axis)
                {
                    nearest.mV[axis] = llclamp(camera.mV[axis], box[0].mV[axis], box[1].mV[axis]);
                }
                distance = (nearest - camera).length();
            }
            else
            {
                distance = (volume->getRenderPosition() - camera).length();
            }
        }
        limit = llmax(limit, asr::avatarDetailTexelLimit(distance));
    }

    // Close-ups sharpen immediately. A brief camera movement should not discard mips.
    if (texture.detailLimit == 0 || limit >= texture.detailLimit || gFrameTimeSeconds - texture.detailLimitAt >= 4.f)
    {
        texture.detailLimit = limit;
        texture.detailLimitAt = gFrameTimeSeconds;
    }
    return asr::textureDiscardForLimit(llmax(image->getFullWidth(), image->getFullHeight()),
                                      texture.detailLimit, image->getMaxDiscardLevel());
}


// Metres from the camera to the nearest thing using this texture. 0 for what always keeps
// full detail: the local avatar, avatars set to always render fully, and the camera's focus
// (an object's whole linkset, or an avatar with what it wears).
static F32 nearestUse(const TrackedTexture& texture)
{
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLViewerObject* focus = gAgentCamera.getFocusObject().get();
    LLViewerObject* focusRoot = focus ? focus->getRootEdit() : nullptr;
    LLViewerObject* focusAvatar = focus ? (focus->isAvatar() ? focus : focus->getAvatar()) : nullptr;
    F32 nearest = F32_MAX;
    for (const auto& user : texture.users)
    {
        LLDrawable* drawable = user.first;
        LLViewerObject* object = drawable ? drawable->getVObj().get() : nullptr;
        if (!object || drawable->isDead()) continue;
        const auto found = sObjects.find(drawable);
        LLVOAvatar* wearer = found != sObjects.end() && found->second.bound ? found->second.bound->avatar : nullptr;
        if (!wearer && object->isAvatar()) wearer = static_cast<LLVOAvatar*>(object); // a system body
        if ((focusRoot && object->getRootEdit() == focusRoot) || (wearer && wearer == focusAvatar) ||
            (wearer && (wearer->isSelf() || alwaysRenderedFully(wearer))))
        {
            return 0.f;
        }
        const LLVector3 center = wearer ? wearer->getRenderPosition() : drawable->getPositionAgent();
        const F32 radius = wearer ? 1.f : drawable->getRadius();
        nearest = llmin(nearest, llmax((center - camera->getOrigin()).length() - radius, 0.f));
    }
    return nearest;
}

F32 ASRWorld::avatarLodScale(LLViewerObject* object)
{
    LLVOAvatar* avatar = sRunning && object ? object->getAvatar() : nullptr;
    const auto found = avatar ? sSkeletons.find(avatar) : sSkeletons.end();
    return found != sSkeletons.end() ? asr::AvatarDetailPolicy::lodScale(found->second.lodCap) : 1.f;
}

bool ASRWorld::isNearTexture(LLViewerFetchedTexture* image)
{
    // In view (the GPU sampled it in the last second) and near. Never refused an update:
    // a blurred or gray surface next to the camera is the worst a crowd can do. Far
    // textures shrink first to make the room (judgeTexture()).
    // Bounded by the working budget itself, not the texture steering target: in a club most
    // of the crowd is within the near range, and unbounded near updates pushed the process into
    // shared memory. The working budget already leaves the desktop and other apps their room.
    const auto found = sTextures.find(image->getGLTexture());
    if (found == sTextures.end()) return false;
    // Within NEAREST_RANGE nothing is refused: little fits there, and it is what is looked at.
    const TrackedTexture& texture = found->second;
    if (texture.slot == 0 || !asr::textureFeedbackSeen(sLastFeedback, texture.gpuSeenAt, 1.0)) return false;
    const F32 distance = nearestUse(texture);
    const asr::MemoryBudget& memory = ASRViewer::device()->memoryBudget();
    return distance <= NEAREST_RANGE || (distance <= sNearMetres && memory.totalUsed() < memory.budget());
}

bool ASRWorld::canRefineVisibleTexture(LLViewerFetchedTexture* image, S32 discard)
{
    const auto found = sTextures.find(image->getGLTexture());
    if (found == sTextures.end() || discard < 0) return false;
    if (isNearTexture(image)) return true;
    const TrackedTexture& texture = found->second;
    // At the limit, far surfaces sharpen only when they fill much of the screen and fit.
    constexpr U8 HIGH_SCREEN_DEMAND = 9; // 512 texels across
    return texture.slot != 0 && texture.latestGpuLevel >= HIGH_SCREEN_DEMAND &&
           asr::textureFeedbackSeen(sLastFeedback, texture.gpuSeenAt, 0.5) &&
           ASR::textureFits(image->getWidth(discard), image->getHeight(discard), image->getComponents(), image->getUseMipMaps());
}

ASRWorld::TextureUse ASRWorld::judgeTexture(LLViewerFetchedTexture* image, bool inView, F32 bias, F32& size)
{
    if (!sRunning)
    {
        return TextureUse::Wanted;
    }

    // A window samples each pixel once, so a texture on a few pixels can miss several. Long
    // enough that one missed for a moment does not drop two levels and come back sharp.
    constexpr F32 SEEN_LATELY = 4.f;      // seconds
    constexpr F32 HIDDEN_DIVISOR = 16.f;  // two levels coarser, so a corner turned shows something
    constexpr F32 PRESSURE_BIAS = 1.9f;   // as the faces' loop: above this, sizes give way

    const auto found = sTextures.find(image->getGLTexture());
    const bool drawn = found != sTextures.end();
    if (drawn && (found->second.slot == 0 || found->second.gpuSeenAt <= -1000.f))
    {
        // GL batching normally marks individual faces rigged and maintains their deformed
        // bounds. It does not run here: the texture walk can test a worn mesh's bind-space
        // box instead of its wearer and assign a near avatar almost no loading priority.
        // Before feedback exists, use the live wearer as a conservative bootstrap bound.
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        for (const auto& user : found->second.users)
        {
            const auto object = sObjects.find(user.first);
            LLVOAvatar* avatar = object != sObjects.end() && object->second.bound ? object->second.bound->avatar : nullptr;
            if (!avatar || avatar->isDead()) continue;
            const LLVector3 center = avatar->getRenderPosition();
            const LLVector3* extents = avatar->getLastAnimExtents();
            const F32 radius = llmax((extents[1] - extents[0]).length() * 0.5f, 1.5f);
            if (camera->sphereInFrustum(center, radius))
            {
                inView = true;
                const F32 distance = llmax((center - camera->getOrigin()).length() - radius, 0.1f);
                const F32 pixels = radius * camera->getPixelMeterRatio() / distance;
                // A small first image gets alpha/BOM surfaces drawing; feedback then
                // requests their actual resolution, without a full-size loading burst.
                size = llmax(size, llmin(F_PI * pixels * pixels, 128.f * 128.f));
            }
        }
    }
    // A complete window needs at least 16 GPU frames. A one-second timeout switched
    // between GPU and face demand at low FPS, undoing the hold and reloading repeatedly.
    // No new completed window means no new evidence that this texture stopped showing.
    if (drawn && sLastFeedback > -1000.f)
    {
        const TrackedTexture& texture = found->second;
        if (asr::textureFeedbackSeen(sLastFeedback, texture.gpuSeenAt, SEEN_LATELY))
        {
            // What the GPU needed, as the area it would cover at full size: the texels across
            // its longer side, and as many down as its shape gives.
            const F32 across = F32(1u << texture.gpuLevel);
            const S32 width = image->getFullWidth();
            const S32 height = image->getFullHeight();
            const F32 longest = F32(llmax(width, height));
            size = longest > 0.f ? across * across * F32(width) * F32(height) / (longest * longest) : across * across;
            if (LLViewerTexture::sDesiredDiscardBias > PRESSURE_BIAS)
            {
                // Room comes from the farthest first: past sNearMetres a surface gives way with
                // the square of its distance. Near ones keep what the GPU asks for; when that is
                // not enough, updateNearRange() narrows the range, so what was near gives way.
                const F32 distance = nearestUse(texture);
                if (distance > sNearMetres)
                {
                    size /= bias * (distance / sNearMetres) * (distance / sNearMetres);
                }
            }
            image->mLastSeen = gFrameTimeSeconds;
            ++sFeedbackSized;
            return TextureUse::Wanted;
        }
        // Never seen by the GPU since it got its slot: no feedback says anything about it yet,
        // so the faces decide, as the viewer would. Judging it hidden starved what cannot show
        // without it: a mesh body cut out by a bake not loaded yet is never sampled, so its
        // bake was never wanted, and avatars in view never rezzed while those behind did.
        if (inView && (texture.slot == 0 || texture.gpuSeenAt <= -1000.f))
        {
            // Feedback cannot prove a surface hidden before it has an image to sample.
            // Refresh its lifetime too: falling through released it despite saying that
            // the faces should decide, cancelling avatar reloads again and again.
            image->mLastSeen = gFrameTimeSeconds;
            return TextureUse::Wanted;
        }
        if (inView)
        {
            // Its faces are in view, but the GPU drew none of it: it is behind something, or
            // too small to show. It is not seen, so under slot pressure it can go.
            size /= HIDDEN_DIVISOR;
            ++sFeedbackHidden;
        }
    }
    else if (drawn && inView)
    {
        image->mLastSeen = gFrameTimeSeconds;
        return TextureUse::Wanted;
    }
    if (gFrameTimeSeconds - image->mLastSeen > sUnseenLimit)
    {
        return TextureUse::Release;
    }
    return drawn ? TextureUse::Wanted : TextureUse::NotDrawn;
}

void ASRWorld::releaseTexture(LLViewerFetchedTexture* image)
{
    if (image->hasGLTexture() && !image->isFetching() && !image->hasCallbacks())
    {
        image->destroyTexture(); // does nothing while a new image is on its way in
        if (!image->hasGLTexture())
        {
            // destroyTexture() marks it deleted, and the texture list then skips its stats
            // for good, so it would never ask for an image again, even back in view. It is
            // only waiting for one, like a texture that has not loaded yet.
            image->setActive();
            ++sReleasedTextures;
        }
    }
}

void ASRWorld::update()
{
    if (!sRunning)
    {
        return;
    }
    updateUnseenLimit();
    takeTextureFeedback();
    timed(PART_MESH, []() { gMeshRepo.notifyLoadedMeshes(); }); // rebuildPriorityGroups() calls this, and it does not run here
    timed(PART_CACHE, cullObjectCache);
    timed(PART_SHAPES, applyFinishedShapes);
    timed(PART_TEXTURES, followTextures);
    ASRTextureCache::update();
    updateLocalShift();
    timed(PART_AVATARS, updateAvatars);
    timed(PART_LOD, updateLods);
    timed(PART_SWEEP, sweepObjects);
    timed(PART_APPLY, evictUnusedMeshLods);
    timed(PART_APPLY, followSelection);
    timed(PART_APPLY, followHudLods);
    timed(PART_APPLY, applyChanges);
    timed(PART_MOVE, []()
    {
        for (LLDrawable* drawable : sMoved)
        {
            moveObject(drawable);
        }
        sMoved.clear();
    });
    timed(PART_TERRAIN, updateTerrain);
    publishCamera();
    timed(PART_AVATARS, updateSystemBodies);
    logCounts();
}

// Reads what LLSelectMgr keeps private: see the friend line in llselectmgr.h.
struct ASRSelectionAccess
{
    // As the test at the top of LLSelectMgr::renderSilhouettes().
    static bool outlinesShown(const LLSelectMgr& manager)
    {
        const bool normal = manager.mFSShowHideHighlight == FS_SHOW_HIDE_HIGHLIGHT_NORMAL;
        return (normal && manager.mRenderSilhouettes && manager.mRenderHighlightSelections) ||
               manager.mFSShowHideHighlight == FS_SHOW_HIDE_HIGHLIGHT_SHOW;
    }
};

void ASRWorld::fillOverlay(asr::OverlayInput& overlay)
{
    overlay = asr::OverlayInput{};
    if (!LLSelectMgr::instanceExists())
    {
        return; // made after the first frames, and the HUD zoom asks it about the selection
    }

    // The HUD zoom eases to its target as render_hud_attachments() eases it, which the
    // native frame does not call: editing a HUD zooms it out as in OpenGL.
    gAgentCamera.mHUDTargetZoom = llclamp(gAgentCamera.mHUDTargetZoom, !gRlvAttachmentLocks.hasLockedHUD() ? 0.1f : 0.85f, 1.f);
    gAgentCamera.mHUDCurZoom = lerp(gAgentCamera.mHUDCurZoom, gAgentCamera.getAgentHUDTargetZoom(), LLSmoothInterpolation::getInterpolant(0.03f));

    // HUD space to clip, as OpenGL's; then Y down and reversed depth, as the renderer's.
    glm::mat4 proj;
    glm::mat4 model;
    overlay.hud = LLPipeline::sShowHUDAttachments && !gDisconnected && get_hud_matrices(proj, model);
    if (overlay.hud)
    {
        const glm::mat4 toRenderer(1.f, 0.f, 0.f, 0.f, 0.f, -1.f, 0.f, 0.f, 0.f, 0.f, -0.5f, 0.f, 0.f, 0.f, 0.5f, 1.f);
        // Drawn in the world view's viewport, as the scene is: see ASRViewer::publishFrame().
        const glm::mat4 hud = toRenderer * proj * model;
        memcpy(overlay.hudViewProjection, glm::value_ptr(hud), sizeof(overlay.hudViewProjection));
    }


    LLSelectMgr* manager = LLSelectMgr::getInstance();
    overlay.outlines = ASRSelectionAccess::outlinesShown(*manager);
    overlay.faceHighlights = manager->getTEMode() && gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_SELECTED);
    overlay.hiddenSelections = LLSelectMgr::sRenderHiddenSelections;
    const LLColor4 root = LLSelectMgr::sSilhouetteParentColor.get();
    const LLColor4 child = LLSelectMgr::sSilhouetteChildColor.get();
    const LLColor4 face(1.f, 1.f, 1.f, 0.3f); // renderSelectedFaces()' white at half, through its striped texture
    for (U32 i = 0; i < 4; ++i)
    {
        overlay.selectionColors[asr::SELECTION_COLOR_ROOT][i] = root.mV[i];
        overlay.selectionColors[asr::SELECTION_COLOR_CHILD][i] = child.mV[i];
        overlay.selectionColors[asr::SELECTION_COLOR_FACE][i] = face.mV[i];
    }
}

#else // the new renderer is not in this build

void ASRWorld::start() {}
void ASRWorld::stop() {}
void ASRWorld::objectChanged(LLDrawable*) {}
void ASRWorld::objectMoved(LLDrawable*) {}
void ASRWorld::objectRemoved(LLDrawable*) {}
void ASRWorld::avatarMasksChanged(LLVOAvatar*) {}
S32 ASRWorld::animationPeriod(LLVOAvatar*) { return 0; }
void ASRWorld::avatarAnimated(LLVOAvatar*) {}
void ASRWorld::updateTerrainGL() {}
void ASRWorld::meshLoaded(const LLVolumeParams&, LLVolume*, S32) {}
void ASRWorld::meshUnavailable(const LLVolumeParams&, S32) {}
void ASRWorld::worldShifted(const LLVector3&) {}
void ASRWorld::addMainTime(MainPart, F32) {}
ASRWorld::TextureUse ASRWorld::judgeTexture(LLViewerFetchedTexture*, bool, F32, F32&) { return TextureUse::Wanted; }
S32 ASRWorld::avatarDetailDiscard(LLViewerFetchedTexture*) { return 0; }
bool ASRWorld::canRefineVisibleTexture(LLViewerFetchedTexture*, S32) { return false; }
void ASRWorld::releaseTexture(LLViewerFetchedTexture*) {}
void ASRWorld::update() {}
void ASRWorld::fillOverlay(asr::OverlayInput&) {}

#endif
