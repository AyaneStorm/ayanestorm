/**
 * @file asravatarbody.cpp
 * @brief See asravatarbody.h.
 * Author: chanayane@firestorm
 */
#include "llviewerprecompiledheaders.h"

#include "asravatarbody.h"

#if defined(AS_RENDERER) && (LL_WINDOWS || LL_DARWIN) // as ASR_VIEWER_SUPPORTED in asrviewer.cpp

#include "asrscene.h"
#include "asrskinweights.h"
#include "asrworldshared.h"
#include "indra_constants.h"
#include "llagent.h"
#include "llavatarjointmesh.h"
#include "lldrawable.h"
#include "llpolymesh.h"
#include "llpolymorph.h"
#include "llviewertexlayer.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "llwearabletype.h"

#include "glm/gtc/type_ptr.hpp"
#include "glm/matrix.hpp"

#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace LLAvatarAppearanceDefines;

namespace
{

constexpr U32 MAX_PALETTE = 15;           // LL_CHARACTER_MAX_JOINTS_PER_MESH, as avatarSkinV.glsl reads it
constexpr F32 MINIMUM_ALPHA = 0.2f;       // LLDrawPoolAvatar::sMinimumAlpha
constexpr F32 LOD_AREA_CHANGE = 0.1f;     // a relative change of pixel area that asks the LODs again
constexpr F32 LOD_RECHECK_SECONDS = 1.f;  // and how long they are trusted without one
constexpr F32 LOCAL_TOLERANCE = 1e-5f;    // a rigid part's matrix, recomputed each frame, moves by rounding

} // namespace

// One body mesh, as every avatar has it.
struct ASRAvatarBody::SharedMesh
{
    bool weighted = false;
    U32 skin = 0;     // weighted
    U32 palette = 0;  // weighted: the skin's entries
    U32 morphSet = 0; // weighted
    std::unordered_map<const LLPolyMorphData*, U32> targets; // the set's target of each morph
    asr::GeometryBox box{};
};

// An avatar's morph of one weighted mesh, and the viewer's params that drive it.
struct ASRAvatarBody::BodyMorph
{
    U32 id = 0;
    std::vector<F32> weights; // as last sent
    std::vector<std::pair<LLPolyMorphTarget*, U32>> params; // and the target each drives
    bool sent = false; // its weights were read at least once
    bool used = false; // a visible part needs this morph in the current update
};

namespace
{

using SharedMesh = ASRAvatarBody::SharedMesh;
using BodyMorph = ASRAvatarBody::BodyMorph;

std::unordered_map<const LLPolyMeshSharedData*, SharedMesh> sMeshes; // by reference mesh
std::unordered_map<const LLPolyMeshSharedData*, U32> sGeometries;    // by LOD mesh, reference included
std::unordered_set<LLVOAvatar*> sMasksChanged;
ASRAvatarBody::Counts sCounts;
U32 sBodies = 0;
U32 sParts = 0;

// One MESH_ID_* part, as last written.
struct Part
{
    U32 instance = 0;
    const LLPolyMeshSharedData* lod = nullptr;
    ASRSharedMaterial* material = nullptr;
    LLViewerTexture* image = nullptr;
    asr::MaterialDesc desc;
    U32 flags = 0;
    U32 joint = 0;     // rigid
    bool eye = false;  // the texture policy can recognize system eye uses
    F32 local[12] = {}; // rigid: in that joint's space
};

LLPolyMeshSharedData* sharedOf(LLPolyMesh* mesh)
{
    return mesh ? mesh->getSharedData() : nullptr;
}

} // namespace

// The mesh whose vertices, weights and morphs a LOD mesh uses.
LLPolyMeshSharedData* ASRAvatarBody::referenceOf(LLPolyMeshSharedData* lod)
{
    return lod && lod->mReferenceData ? lod->mReferenceData : lod;
}

namespace
{

// The skeleton joint something hangs from: the nearest ancestor the avatar numbers.
LLJoint* skeletonJointAbove(LLVOAvatar* avatar, LLJoint* joint)
{
    for (LLJoint* parent = joint ? joint->getParent() : nullptr; parent; parent = parent->getParent())
    {
        const S32 number = parent->getJointNum();
        if (number >= 0 && avatar->getJoint(number) == parent)
        {
            return parent;
        }
    }
    return nullptr;
}

F32 tangentOf(const F32* normal, const F32* binormal, F32* tangent)
{
    // As morphVertex() makes it: the binormal crossed with the normal.
    tangent[0] = binormal[1] * normal[2] - binormal[2] * normal[1];
    tangent[1] = binormal[2] * normal[0] - binormal[0] * normal[2];
    tangent[2] = binormal[0] * normal[1] - binormal[1] * normal[0];
    const F32 length = sqrtf(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
    if (length > 0.f)
    {
        tangent[0] /= length;
        tangent[1] /= length;
        tangent[2] /= length;
    }
    else
    {
        tangent[0] = 1.f;
        tangent[1] = tangent[2] = 0.f;
    }
    tangent[3] = 1.f;
    return length;
}

// The skin LLViewerJointMesh::uploadJointMatrices() makes: each entry of the reference mesh's
// joint list is its joint's world matrix after a move by its pivot, which is the inverse bind.
// A parent entry, with no skin joint, takes the pivot of the entry after it.
U32 makeSkin(LLVOAvatar* avatar, LLPolyMesh* mesh)
{
    const std::vector<LLJointRenderData*>& entries = mesh->mJointRenderData;
    const U32 count = U32(llmin(entries.size(), size_t(MAX_PALETTE)));
    if (count == 0)
    {
        return 0;
    }
    asr::SkinDesc desc;
    desc.matrices.assign(size_t(count) * 12, 0.f);
    desc.joints.assign(count, 0);
    for (U32 i = 0; i < count; ++i)
    {
        LLVector3 pivot;
        LLJoint* joint = nullptr;
        if (const LLSkinJoint* skin = entries[i]->mSkinJoint)
        {
            pivot = skin->mRootToJointSkinOffset;
            joint = skin->mJoint;
        }
        else if (i + 1 < entries.size() && entries[i + 1]->mSkinJoint)
        {
            const LLSkinJoint* next = entries[i + 1]->mSkinJoint;
            pivot = next->mRootToParentJointSkinOffset;
            joint = next->mJoint ? next->mJoint->getParent() : nullptr;
            while (joint && &joint->getWorldMatrix() != entries[i]->mWorldMatrix)
            {
                joint = joint->getParent();
            }
        }
        if (!joint || joint->getJointNum() < 0 || avatar->getJoint(joint->getJointNum()) != joint)
        {
            LL_WARNS("ASRenderer") << "body mesh joint " << i << " is not in the skeleton" << LL_ENDL;
            joint = nullptr;
        }
        F32* rows = &desc.matrices[size_t(i) * 12];
        rows[0] = rows[5] = rows[10] = 1.f;
        rows[3] = pivot.mV[VX];
        rows[7] = pivot.mV[VY];
        rows[11] = pivot.mV[VZ];
        desc.joints[i] = joint ? U16(joint->getJointNum()) : 0;
    }
    return ASRWorldShared::scene().addSkin(std::move(desc));
}

} // namespace

// The morph set of a reference mesh: its base vertices and every morph target it has.
// A target the viewer masks is masked here, which the first avatar's params tell: masks
// come from avatar_lad.xml, so every avatar has the same ones.
U32 ASRAvatarBody::makeMorphSet(LLVOAvatar* avatar, LLPolyMesh* mesh, SharedMesh& shared)
{
    LLPolyMeshSharedData* data = mesh->getSharedData();
    std::unordered_set<const LLPolyMorphData*> masked;
    for (LLVisualParam* param = avatar->getFirstVisualParam(); param; param = avatar->getNextVisualParam())
    {
        for (LLVisualParam* each = param; each; each = each->getNextParam())
        {
            LLPolyMorphTarget* target = dynamic_cast<LLPolyMorphTarget*>(each);
            if (target && target->mMesh == mesh && target->mMorphData && (target->mVertMask || target->mNumMorphMasksPending > 0))
            {
                masked.insert(target->mMorphData);
            }
        }
    }

    asr::MorphSetDesc desc;
    const U32 vertices = U32(data->mNumVertices);
    desc.positions.resize(size_t(vertices) * 3);
    desc.normals.resize(size_t(vertices) * 3);
    desc.binormals.resize(size_t(vertices) * 3);
    desc.uvs.resize(size_t(vertices) * 2);
    for (U32 v = 0; v < vertices; ++v)
    {
        memcpy(&desc.positions[size_t(v) * 3], data->mBaseCoords[v].getF32ptr(), 3 * sizeof(F32));
        memcpy(&desc.normals[size_t(v) * 3], data->mBaseNormals[v].getF32ptr(), 3 * sizeof(F32));
        memcpy(&desc.binormals[size_t(v) * 3], data->mBaseBinormals[v].getF32ptr(), 3 * sizeof(F32));
        desc.uvs[size_t(v) * 2] = data->mTexCoords[v].mV[VX];
        desc.uvs[size_t(v) * 2 + 1] = data->mTexCoords[v].mV[VY];
    }

    for (LLPolyMorphData* morph : data->mMorphData)
    {
        shared.targets[morph] = U32(desc.targets.size());
        asr::MorphTargetDesc target;
        const U32 count = morph->mNumIndices;
        target.vertices.assign(morph->mVertexIndices, morph->mVertexIndices + count);
        target.positions.resize(size_t(count) * 3);
        target.normals.resize(size_t(count) * 3);
        target.binormals.resize(size_t(count) * 3);
        target.uvs.resize(size_t(count) * 2);
        for (U32 i = 0; i < count; ++i)
        {
            memcpy(&target.positions[size_t(i) * 3], morph->mCoords[i].getF32ptr(), 3 * sizeof(F32));
            memcpy(&target.normals[size_t(i) * 3], morph->mNormals[i].getF32ptr(), 3 * sizeof(F32));
            // As apply() guards it: a degenerate binormal delta counts as (1, 0, 0).
            LLVector4a binormal = morph->mBinormals[i];
            if (!binormal.isFinite3() || binormal.dot3(binormal).getF32() <= F_APPROXIMATELY_ZERO)
            {
                binormal.set(1.f, 0.f, 0.f, 1.f);
            }
            memcpy(&target.binormals[size_t(i) * 3], binormal.getF32ptr(), 3 * sizeof(F32));
            target.uvs[size_t(i) * 2] = morph->mTexCoords[i].mV[VX];
            target.uvs[size_t(i) * 2 + 1] = morph->mTexCoords[i].mV[VY];
        }
        target.masked = masked.count(morph) != 0;
        desc.targets.push_back(std::move(target));
    }
    return ASRWorldShared::scene().addMorphSet(std::move(desc));
}

ASRAvatarBody::SharedMesh& ASRAvatarBody::sharedMesh(LLVOAvatar* avatar, LLPolyMesh* reference)
{
    LLPolyMeshSharedData* data = reference->getSharedData();
    auto found = sMeshes.find(data);
    if (found != sMeshes.end())
    {
        return found->second;
    }
    SharedMesh& shared = sMeshes[data];

    // One box for every LOD, as they share the vertices.
    LLVector4a low(F32_MAX, F32_MAX, F32_MAX);
    LLVector4a high(-F32_MAX, -F32_MAX, -F32_MAX);
    for (S32 v = 0; v < data->mNumVertices; ++v)
    {
        low.setMin(low, data->mBaseCoords[v]);
        high.setMax(high, data->mBaseCoords[v]);
    }
    for (U32 axis = 0; axis < 3; ++axis)
    {
        shared.box.center[axis] = data->mNumVertices > 0 ? (low[axis] + high[axis]) * 0.5f : 0.f;
    }
    shared.box.scale = data->mNumVertices > 0 ? llmax((high[0] - low[0]) * 0.5f, (high[1] - low[1]) * 0.5f, (high[2] - low[2]) * 0.5f, 1e-6f) : 1.f;

    shared.weighted = data->mWeights != nullptr;
    if (shared.weighted)
    {
        shared.skin = makeSkin(avatar, reference);
        shared.palette = U32(llmin(reference->mJointRenderData.size(), size_t(MAX_PALETTE)));
        shared.morphSet = makeMorphSet(avatar, reference, shared);
    }
    return shared;
}

// A LOD's geometry: the reference mesh's vertices it reaches, with its own triangles.
U32 ASRAvatarBody::lodGeometry(LLPolyMeshSharedData* lod, const SharedMesh& shared)
{
    auto found = sGeometries.find(lod);
    if (found != sGeometries.end())
    {
        return found->second;
    }
    U32& geometry = sGeometries[lod];
    const LLPolyMeshSharedData* reference = referenceOf(lod);
    const U32 vertexCount = U32(llmin(lod->mNumVertices, reference->mNumVertices));
    if (vertexCount == 0 || vertexCount > 65536 || lod->mNumFaces <= 0)
    {
        LL_WARNS("ASRenderer") << "body mesh LOD left out: " << vertexCount << " vertices, " << lod->mNumFaces << " faces" << LL_ENDL;
        return geometry;
    }

    std::vector<asr::SceneVertex> vertices(vertexCount);
    for (U32 v = 0; v < vertexCount; ++v)
    {
        asr::SceneVertex& vertex = vertices[v];
        memcpy(vertex.position, reference->mBaseCoords[v].getF32ptr(), sizeof(vertex.position));
        memcpy(vertex.normal, reference->mBaseNormals[v].getF32ptr(), sizeof(vertex.normal));
        tangentOf(vertex.normal, reference->mBaseBinormals[v].getF32ptr(), vertex.tangent);
        vertex.uv[0] = reference->mTexCoords[v].mV[VX];
        vertex.uv[1] = reference->mTexCoords[v].mV[VY];
    }

    std::vector<U16> indices;
    indices.reserve(size_t(lod->mNumFaces) * 3);
    for (S32 face = 0; face < lod->mNumFaces; ++face)
    {
        const S32* corners = lod->mFaces[face];
        if (corners[0] < S32(vertexCount) && corners[1] < S32(vertexCount) && corners[2] < S32(vertexCount) &&
            corners[0] >= 0 && corners[1] >= 0 && corners[2] >= 0)
        {
            indices.push_back(U16(corners[0]));
            indices.push_back(U16(corners[1]));
            indices.push_back(U16(corners[2]));
        }
    }

    // As avatarSkinV.glsl blends: joint floor(w) and the one after, by fract(w). A joint past
    // the skin's entries is held to its last one: the skin has no more, and a geometry naming
    // more joints than its skin is not drawn at all.
    std::vector<asr::SkinWeights> weights;
    if (shared.weighted)
    {
        weights.resize(vertexCount);
        const U32 last = llmax(shared.palette, 1u) - 1;
        for (U32 v = 0; v < vertexCount; ++v)
        {
            const F32 w = reference->mWeights[v];
            const F32 whole = floorf(w);
            const U32 first = U32(llclamp(S32(whole), 0, S32(last)));
            const U32 next = llmin(first + 1, last);
            const F32 shares[4] = { 1.f - (w - whole), w - whole, 0.f, 0.f };
            weights[v].joints[0] = U8(first);
            weights[v].joints[1] = U8(next);
            weights[v].joints[2] = weights[v].joints[3] = 0;
            asr::packSkinAmounts(shares, weights[v].weights);
        }
    }
    geometry = ASRWorldShared::scene().addGeometry(asr::buildGeometry(vertices.data(), vertexCount, indices.data(), U32(indices.size()), &shared.box,
                                                                      weights.empty() ? nullptr : weights.data()));
    return geometry;
}

namespace
{

// Which LOD of a part shows: the first valid one, as LLViewerJoint::render() picks it after
// LLAvatarJoint::updateLOD().
LLAvatarJointMesh* shownLod(LLAvatarJoint* part)
{
    for (LLAvatarJointMesh* mesh : part->mMeshParts)
    {
        if (mesh && mesh->getValid())
        {
            return mesh;
        }
    }
    return nullptr;
}

// LLVOAvatar::renderSkinned(), renderTransparent() and renderRigid(), part by part.
bool partShown(LLVOAvatar* avatar, S32 id, bool hideHead)
{
    const LLVOAvatar::AvatarOverallAppearance appearance = avatar->getOverallAppearance();
    const bool jelly = appearance == LLVOAvatar::AOA_JELLYDOLL && !avatar->isControlAvatar();
    switch (id)
    {
        case MESH_ID_HEAD:
            return !hideHead && (avatar->isTextureVisible(TEX_HEAD_BAKED) || jelly);
        case MESH_ID_UPPER_BODY:
            return avatar->isTextureVisible(TEX_UPPER_BAKED) || jelly;
        case MESH_ID_LOWER_BODY:
            return avatar->isTextureVisible(TEX_LOWER_BAKED) || jelly;
        case MESH_ID_SKIRT:
            return avatar->isWearingWearableType(LLWearableType::WT_SKIRT) && avatar->isTextureVisible(TEX_SKIRT_BAKED);
        case MESH_ID_EYELASH:
            return !hideHead && avatar->isTextureVisible(TEX_HEAD_BAKED);
        case MESH_ID_HAIR:
            return !hideHead && avatar->isTextureVisible(TEX_HAIR_BAKED) && !jelly;
        case MESH_ID_EYEBALL_LEFT:
        case MESH_ID_EYEBALL_RIGHT:
            return !hideHead && (avatar->isTextureVisible(TEX_EYES_BAKED) || jelly);
        default:
            return false;
    }
}

} // namespace

// The image LLViewerJointMesh::drawShape() binds, and its colour.
LLViewerTexture* ASRAvatarBody::partImage(LLVOAvatar* avatar, LLAvatarJointMesh* mesh, asr::MaterialDesc& desc)
{
    if (avatar->getOverallAppearance() == LLVOAvatar::AOA_JELLYDOLL)
    {
        // The viewer draws these as a flat silhouette in this colour.
        const LLColor4& color = avatar->getMutedAVColor();
        memcpy(desc.color, color.mV, sizeof(desc.color));
        desc.color[3] = 1.f;
        return nullptr;
    }
    memcpy(desc.color, mesh->mColor.mV, sizeof(desc.color));
    if (LLViewerTexLayerSet* layers = dynamic_cast<LLViewerTexLayerSet*>(mesh->mLayerSet))
    {
        if (layers->hasComposite())
        {
            return layers->getViewerComposite();
        }
    }
    else if (mesh->mTexture.notNull())
    {
        return dynamic_cast<LLViewerTexture*>(mesh->mTexture.get());
    }
    return LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT);
}

namespace
{

U32 partFlags(S32 id, asr::MaterialDesc& desc)
{
    switch (id)
    {
        case MESH_ID_HAIR:
        case MESH_ID_SKIRT:
            return asr::INSTANCE_BLENDED | asr::INSTANCE_DOUBLE_SIDED; // isTransparent(): no culling
        case MESH_ID_EYELASH:
            return asr::INSTANCE_BLENDED;
        default:
            desc.alphaCutoff = MINIMUM_ALPHA;
            return asr::INSTANCE_MASKED;
    }
}

bool sameRows(const F32* a, const F32* b)
{
    for (U32 i = 0; i < 12; ++i)
    {
        if (fabsf(a[i] - b[i]) > LOCAL_TOLERANCE)
        {
            return false;
        }
    }
    return true;
}

} // namespace

struct ASRAvatarBody::Body
{
    LLVOAvatar* avatar = nullptr; // only a key once released: see release()
    LLDrawable* user = nullptr;   // whose textures they are
    U32 skeleton = 0;
    std::unordered_map<const LLPolyMeshSharedData*, BodyMorph> morphs; // by reference mesh
    Part parts[MESH_ID_NUM_INDICES];
    F32 lodArea = -1.f;
    F32 lodCheckedAt = -1000.f;
    bool masksSent = false;
};

void ASRAvatarBody::BodyDeleter::operator()(Body* body) const
{
    delete body;
}

namespace
{

} // namespace

ASRAvatarBody::BodyMorph& ASRAvatarBody::bodyMorph(Body& body, LLPolyMesh* reference, const SharedMesh& shared)
{
    BodyMorph& morph = body.morphs[reference->getSharedData()];
    morph.used = true;
    if (morph.id != 0)
    {
        return morph;
    }
    morph.id = ASRWorldShared::scene().addMorph(shared.morphSet);
    morph.weights.assign(shared.targets.size(), 0.f);
    LLVOAvatar* avatar = body.avatar;
    for (LLVisualParam* param = avatar->getFirstVisualParam(); param; param = avatar->getNextVisualParam())
    {
        for (LLVisualParam* each = param; each; each = each->getNextParam())
        {
            LLPolyMorphTarget* target = dynamic_cast<LLPolyMorphTarget*>(each);
            if (!target || target->mMesh != reference || !target->mMorphData)
            {
                continue;
            }
            const auto index = shared.targets.find(target->mMorphData);
            if (index != shared.targets.end())
            {
                morph.params.emplace_back(target, index->second);
            }
        }
    }
    body.masksSent = false; // a new morph has none yet
    return morph;
}

namespace
{

// What the viewer's mesh holds: each param's last applied weight, which already follows the
// avatar's sex and waits for pending masks.
void sendWeights(BodyMorph& morph)
{
    morph.sent = true;
    static std::vector<F32> weights; // reused: the main thread only
    weights.assign(morph.weights.size(), 0.f);
    for (const auto& [target, index] : morph.params)
    {
        const F32 weight = target->getLastWeight();
        weights[index] += llisnan(weight) ? 0.f : weight;
    }
    if (weights != morph.weights)
    {
        morph.weights.swap(weights);
        ASRWorldShared::scene().setMorphWeights(morph.id, morph.weights.data(), U32(morph.weights.size()));
        ++sCounts.weightUpdates;
    }
}

} // namespace

void ASRAvatarBody::sendMasks(BodyMorph& morph)
{
    for (const auto& [target, index] : morph.params)
    {
        F32* values = target->mVertMask ? target->mVertMask->getMorphMaskWeights() : nullptr;
        if (values)
        {
            ASRWorldShared::scene().setMorphMask(morph.id, index, values, target->mMorphData->mNumIndices);
            ++sCounts.maskUpdates;
        }
    }
}

namespace
{

void clearPart(ASRAvatarBody::Body& body, Part& part)
{
    if (part.instance != 0)
    {
        ASRWorldShared::scene().removeInstance(part.instance);
        --sParts;
    }
    if (part.material)
    {
        ASRWorldShared::releaseMaterial(part.material, body.user, part.eye);
    }
    part = Part();
}

} // namespace

void ASRAvatarBody::updatePart(Body& body, S32 id, bool show)
{
    LLVOAvatar* avatar = body.avatar;
    Part& part = body.parts[id];
    LLAvatarJoint* joint = id < S32(avatar->mMeshLOD.size()) ? avatar->mMeshLOD[id] : nullptr;
    LLAvatarJointMesh* mesh = show && joint && joint->mVisible ? shownLod(joint) : nullptr;
    LLPolyMesh* lodMesh = mesh && static_cast<LLAvatarJoint*>(mesh)->mVisible ? mesh->getMesh() : nullptr;
    LLPolyMeshSharedData* lod = sharedOf(lodMesh);
    if (!lod || lodMesh->getNumFaces() == 0)
    {
        clearPart(body, part);
        return;
    }

    LLPolyMesh* reference = lodMesh->getReferenceMesh();
    const SharedMesh& shared = sharedMesh(avatar, reference);
    const U32 geometry = lodGeometry(lod, shared);
    if (geometry == 0 || (shared.weighted && (shared.skin == 0 || shared.morphSet == 0)))
    {
        clearPart(body, part);
        return;
    }

    asr::InstanceDesc desc;
    desc.geometry = geometry;
    desc.skeleton = body.skeleton;
    bool moved = false;
    if (shared.weighted)
    {
        // Its weights are sent after the parts, the same frame: see update().
        BodyMorph& morph = bodyMorph(body, reference, shared);
        desc.skin = shared.skin;
        desc.morph = morph.id;
    }
    else
    {
        // Drawn rigidly with the joint mesh's own matrix: placed in its skeleton joint's space.
        LLJoint* above = skeletonJointAbove(avatar, mesh);
        if (!above)
        {
            clearPart(body, part);
            return;
        }
        // A general inverse: joints scale (eye size), which LLMatrix4::invert() does not take.
        const glm::mat4 jointWorld = glm::make_mat4(&above->getWorldMatrix().mMatrix[0][0]);
        const glm::mat4 meshWorld = glm::make_mat4(&mesh->getWorldMatrix().mMatrix[0][0]);
        const glm::mat4 relative = glm::inverse(jointWorld) * meshWorld; // column vectors: mesh, then to joint
        LLMatrix4 local(glm::value_ptr(relative));
        F32 rows[12];
        ASRWorldShared::rowsOf(local, rows);
        desc.joint = U32(above->getJointNum());
        if (part.instance == 0 || part.joint != desc.joint || !sameRows(rows, part.local))
        {
            memcpy(part.local, rows, sizeof(rows));
            moved = true;
        }
        memcpy(desc.objectToWorld, part.local, sizeof(part.local));
    }

    asr::MaterialDesc material;
    LLViewerTexture* image = partImage(avatar, mesh, material);
    desc.flags = partFlags(id, material);
    const bool newMaterial = !part.material || image != part.image || memcmp(material.color, part.desc.color, sizeof(material.color)) != 0 ||
                             material.alphaCutoff != part.desc.alphaCutoff;
    if (newMaterial)
    {
        ASRSharedMaterial* previous = part.material;
        part.eye = id == MESH_ID_EYEBALL_LEFT || id == MESH_ID_EYEBALL_RIGHT;
        part.material = ASRWorldShared::useMaterial(material, image, body.user, part.eye);
        if (previous)
        {
            ASRWorldShared::releaseMaterial(previous, body.user, part.eye);
        }
        part.image = image;
        part.desc = material;
    }
    desc.material = ASRWorldShared::materialId(part.material);
    ASRWorldShared::visitMaterialTexture(part.material);

    if (part.instance == 0)
    {
        part.instance = ASRWorldShared::scene().addInstance(desc);
        ++sParts;
    }
    else if (newMaterial || moved || part.lod != lod || part.flags != desc.flags)
    {
        ASRWorldShared::scene().setInstance(part.instance, desc);
    }
    if (part.lod != nullptr && part.lod != lod)
    {
        ++sCounts.lodChanges;
    }
    part.lod = lod;
    part.flags = desc.flags;
    part.joint = desc.joint;
}


bool ASRAvatarBody::wanted(LLVOAvatar* avatar)
{
    return avatar && !avatar->isDead() && avatar->isBuilt() && !avatar->isControlAvatar() && !avatar->isUIAvatar() && avatar->mDrawable.notNull();
}

void ASRAvatarBody::update(LLVOAvatar* avatar, BodyPtr& body, U32 skeleton, bool posed)
{
    if (!body)
    {
        posed = true; // its first weights
        body.reset(new Body());
        body->avatar = avatar;
        body->user = avatar->mDrawable.get();
        ++sBodies;
    }
    body->skeleton = skeleton;

    // LODs from pixel area, as LLVOAvatar::updateLOD() would choose them, which native
    // rendering no longer runs: asked again when the area moves by a tenth, or a second on.
    const F32 area = avatar->getPixelArea();
    if (fabsf(area - body->lodArea) > body->lodArea * LOD_AREA_CHANGE || gFrameTimeSeconds - body->lodCheckedAt > LOD_RECHECK_SECONDS)
    {
        avatar->updateJointLODs();
        body->lodArea = area;
        body->lodCheckedAt = gFrameTimeSeconds;
    }

    // Nothing at all: the viewer's own avatar in some camera modes, avatars still loading
    // (the viewer shows a cloud), and avatars the viewer makes invisible.
    const bool self = avatar->isSelf();
    // Match the viewer's visibility rules. An unrelated slow attachment must not
    // impose another ten-second wait on otherwise loaded system body parts.
    const bool none = (self && !gAgent.needsRenderAvatar()) || !avatar->isFullyLoaded() ||
                      avatar->getOverallAppearance() == LLVOAvatar::AOA_INVISIBLE;
    const bool hideHead = self && !gAgent.needsRenderHead();
    for (auto& [mesh, morph] : body->morphs) morph.used = false;
    for (S32 id = 0; id < MESH_ID_NUM_INDICES; ++id)
    {
        updatePart(*body, id, !none && partShown(avatar, id, hideHead));
    }

    // Hidden system parts need no private morphed output or mask storage. A returning
    // part recreates it from current appearance values, with retirement handled by the store.
    for (auto next = body->morphs.begin(); next != body->morphs.end();)
    {
        BodyMorph& morph = next->second;
        if (!morph.used)
        {
            ASRWorldShared::scene().removeMorph(morph.id);
            next = body->morphs.erase(next);
            continue;
        }
        if (posed || !morph.sent)
        {
            sendWeights(morph);
        }
        ++next;
    }
    const auto changed = sMasksChanged.find(avatar);
    if (changed != sMasksChanged.end() || !body->masksSent)
    {
        for (auto& [mesh, morph] : body->morphs)
        {
            sendMasks(morph);
        }
        body->masksSent = true;
        if (changed != sMasksChanged.end())
        {
            sMasksChanged.erase(changed);
        }
    }
}

void ASRAvatarBody::release(BodyPtr& body)
{
    if (!body)
    {
        return;
    }
    for (Part& part : body->parts)
    {
        clearPart(*body, part);
    }
    for (auto& [mesh, morph] : body->morphs)
    {
        ASRWorldShared::scene().removeMorph(morph.id);
    }
    sMasksChanged.erase(body->avatar);
    body.reset();
    --sBodies;
}

void ASRAvatarBody::masksChanged(LLVOAvatar* avatar)
{
    // Only avatars that get a body take them: update() clears the mark, release() too.
    if (wanted(avatar))
    {
        sMasksChanged.insert(avatar);
    }
}

void ASRAvatarBody::forget()
{
    sMeshes.clear();
    sGeometries.clear();
    sMasksChanged.clear();
    sCounts = Counts();
    sBodies = 0;
    sParts = 0;
}

ASRAvatarBody::Counts ASRAvatarBody::takeCounts()
{
    Counts counts = sCounts;
    counts.bodies = sBodies;
    counts.parts = sParts;
    sCounts = Counts();
    return counts;
}

#else

void ASRAvatarBody::BodyDeleter::operator()(Body*) const {}
bool ASRAvatarBody::wanted(LLVOAvatar*) { return false; }
void ASRAvatarBody::update(LLVOAvatar*, BodyPtr&, U32, bool) {}
void ASRAvatarBody::release(BodyPtr&) {}
void ASRAvatarBody::masksChanged(LLVOAvatar*) {}
void ASRAvatarBody::forget() {}
ASRAvatarBody::Counts ASRAvatarBody::takeCounts() { return Counts(); }

#endif
