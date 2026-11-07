/**
 * @file asrworld.cpp
 * @brief See asrworld.h.
 */
#include "llviewerprecompiledheaders.h"

#include "asrworld.h"

#if defined(AS_RENDERER) && LL_WINDOWS

#include "asrbridge.h"
#include "asrdevice.h"
#include "asrviewer.h"
#include "lldrawable.h"
#include "llfetchedgltfmaterial.h"
#include "llmaterial.h"
#include "lltextureentry.h"
#include "lltoolpie.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvocache.h"
#include "llvolume.h"
#include "llvolumemgr.h"
#include "llvovolume.h"
#include "llworld.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

constexpr U32 NO_INSTANCE = 0xFFFFFFFFu;
constexpr U32 LODS = LLVolumeLODGroup::NUM_LODS;
constexpr U32 MAX_WORKERS = 4;
constexpr auto LOG_PERIOD = std::chrono::seconds(20);

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
};

struct Shape
{
    explicit Shape(const LLVolumeParams& volumeParams) : params(volumeParams) {}

    LLVolumeParams params;
    std::vector<ShapeFace> faces;
    LLVector3 lodBias{ 1.f, 1.f, 1.f };
    U32 users = 0;
    bool ready = false;                      // set by the main thread, once a worker is done
    std::unordered_set<LLDrawable*> waiting; // objects to apply once it is ready
};

struct SharedMaterial;

// A texture some face shows. Its slot changes as sharper versions load.
struct TrackedTexture
{
    LLPointer<LLViewerTexture> image;
    U32 slot = 0;
    S8 components = 0;
    U32 changes = 0; // temporary: how often its slot changed
    std::unordered_map<LLDrawable*, U32> users; // how many of its faces use it
    std::vector<SharedMaterial*> materials;     // what a new slot has to be written to
};

// What makes two materials one. The slot is not part of it: it follows the image.
struct MaterialKey
{
    F32 values[17];
    U32 planar;
    const LLViewerTexture* image;

    bool operator==(const MaterialKey& other) const { return memcmp(this, &other, sizeof(MaterialKey)) == 0; }
};
static_assert(sizeof(MaterialKey) == 80, "compared and hashed as bytes, so it has no padding");

struct SharedMaterial
{
    U32 id = 0;
    U32 users = 0; // faces
    asr::MaterialDesc desc;
    TrackedTexture* texture = nullptr;
};

struct ObjectFace
{
    U32 instance = NO_INSTANCE;
    SharedMaterial* material = nullptr;
    U32 flags = 0;
    U32 rank = 0;        // see resolveStack()
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

struct WorldObject
{
    Shape* shape = nullptr;
    std::vector<ObjectFace> faces;
    Stack* stack = nullptr;
    StackKey stackKey{};
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

std::map<LLVolumeParams, std::unique_ptr<Shape>> sShapes;
std::unordered_map<const void*, TrackedTexture> sTextures; // by LLImageGL, which is what reports a change
std::unordered_map<MaterialKey, SharedMaterial, BytesHash<MaterialKey>> sMaterials;
std::unordered_map<LLDrawable*, WorldObject> sObjects;
std::unordered_map<StackKey, Stack, BytesHash<StackKey>> sStacks;
std::unordered_map<LLDrawable*, const char*> sLeftOut; // what is not drawn yet, and why
std::unordered_set<LLDrawable*> sChanged;
std::unordered_set<LLDrawable*> sMoved;
std::vector<const void*> sChangedImages;
std::chrono::steady_clock::time_point sLogged;

// Test tool: ASRendererLocalShift lifts the last clicked object in this window only, to
// show what sits behind it. A raw pointer, cleared in objectRemoved().
LLDrawable* sShifted = nullptr;
F32 sShift = 0.f;
U32 sLostTextures = 0; // temporary: textures that lost their image with faces on them

// Workers: they touch only their own volumes and the scene queue.
std::vector<std::thread> sWorkers;
std::mutex sJobMutex;
std::condition_variable sJobReady;
std::deque<Shape*> sJobs;
std::vector<Shape*> sFinished;
bool sStopping = false;

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

    std::vector<asr::SceneVertex> vertices;
    U32 coarser = 0;
    for (U32 lod = 0; lod < LODS; ++lod)
    {
        LLVolumeFace* volumeFace = lods[lod];
        if (!volumeFace)
        {
            continue;
        }
        volumeFace->createTangents();

        vertices.resize(volumeFace->mNumVertices);
        for (S32 i = 0; i < volumeFace->mNumVertices; ++i)
        {
            asr::SceneVertex& vertex = vertices[i];
            memcpy(vertex.position, volumeFace->mPositions[i].getF32ptr(), sizeof(vertex.position));
            memcpy(vertex.normal, volumeFace->mNormals[i].getF32ptr(), sizeof(vertex.normal));
            memcpy(vertex.tangent, volumeFace->mTangents[i].getF32ptr(), sizeof(vertex.tangent));
            memcpy(vertex.uv, volumeFace->mTexCoords[i].mV, sizeof(vertex.uv));
        }
        coarser = sScene->addGeometry(asr::buildGeometry(vertices.data(), U32(vertices.size()), volumeFace->mIndices, U32(volumeFace->mNumIndices), &face.box),
                                      coarser, COARSER_AT[lod]);
        face.geometries[lod] = coarser;
    }
    face.finest = coarser;
}

void buildShape(Shape& shape)
{
    LLPointer<LLVolume> volumes[LODS];
    for (U32 lod = 0; lod < LODS; ++lod)
    {
        volumes[lod] = new LLVolume(shape.params, LLVolumeLODGroup::getVolumeScaleFromDetail(lod));
    }

    shape.lodBias = volumes[LODS - 1]->mLODScaleBias;
    shape.faces.resize(volumes[LODS - 1]->getNumVolumeFaces());
    for (S32 index = 0; index < S32(shape.faces.size()); ++index)
    {
        LLVolumeFace* lods[LODS] = {};
        bool any = false;
        for (U32 lod = 0; lod < LODS; ++lod)
        {
            if (index < volumes[lod]->getNumVolumeFaces())
            {
                LLVolumeFace& volumeFace = volumes[lod]->getVolumeFace(index);
                if (volumeFace.mNumVertices > 0 && volumeFace.mNumIndices >= 3)
                {
                    lods[lod] = &volumeFace;
                    any = true;
                }
            }
        }
        if (any)
        {
            addFaceGeometry(shape.faces[index], lods);
        }
    }
}

void workerLoop()
{
    while (true)
    {
        Shape* shape = nullptr;
        {
            std::unique_lock<std::mutex> lock(sJobMutex);
            sJobReady.wait(lock, []() { return sStopping || !sJobs.empty(); });
            if (sStopping)
            {
                return;
            }
            shape = sJobs.front();
            sJobs.pop_front();
        }
        buildShape(*shape);
        std::lock_guard<std::mutex> lock(sJobMutex);
        sFinished.push_back(shape);
    }
}

Shape* useShape(const LLVolumeParams& params)
{
    auto found = sShapes.find(params);
    if (found == sShapes.end())
    {
        found = sShapes.emplace(params, std::make_unique<Shape>(params)).first;
        std::lock_guard<std::mutex> lock(sJobMutex);
        sJobs.push_back(found->second.get());
        sJobReady.notify_one();
    }
    ++found->second->users;
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
    sShapes.erase(shape->params);
}

// A shape a worker still holds goes when it comes back: see applyFinishedShapes().
void releaseShape(Shape* shape, LLDrawable* drawable)
{
    shape->waiting.erase(drawable);
    if (--shape->users == 0 && shape->ready)
    {
        forgetShape(shape);
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

TrackedTexture* useTexture(LLViewerTexture* image, LLDrawable* drawable)
{
    auto found = sTextures.find(imageOf(image));
    if (found == sTextures.end())
    {
        found = sTextures.emplace(imageOf(image), TrackedTexture()).first;
        found->second.image = image;
        found->second.slot = ASR::textureSlot(image->getTexName());
        found->second.components = image->getComponents();
    }
    ++found->second.users[drawable];
    return &found->second;
}

void releaseTexture(TrackedTexture* texture, LLDrawable* drawable)
{
    auto user = texture->users.find(drawable);
    if (user != texture->users.end() && --user->second == 0)
    {
        texture->users.erase(user);
    }
    if (texture->users.empty())
    {
        sTextures.erase(imageOf(texture->image.get()));
    }
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

SharedMaterial* useMaterial(const asr::MaterialDesc& desc, LLViewerTexture* image, LLDrawable* drawable)
{
    TrackedTexture* texture = image ? useTexture(image, drawable) : nullptr;
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

void releaseMaterial(SharedMaterial* material, LLDrawable* drawable)
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
        releaseTexture(texture, drawable);
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

void setUVTransform(asr::MaterialDesc& material, F32 c, F32 s, F32 scaleS, F32 scaleT, F32 offsetS, F32 offsetT)
{
    // LLFace's xform() as a matrix: rotate about the centre, then scale, then offset.
    material.uvRow0[0] = scaleS * c;
    material.uvRow0[1] = scaleS * s;
    material.uvRow0[2] = -0.5f * scaleS * (c + s) + offsetS + 0.5f;
    material.uvRow1[0] = -scaleT * s;
    material.uvRow1[1] = scaleT * c;
    material.uvRow1[2] = 0.5f * scaleT * (s - c) + offsetT + 0.5f;
}

// Blending comes with the transparency pass. Until then a blended surface shows where
// it is more than half opaque, so glass and hair do not turn solid.
constexpr F32 BLEND_STANDIN_CUTOFF = 0.5f;

// The material of a face. Returns false when the face shows nothing at all.
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
            flags |= asr::INSTANCE_MASKED;
            material.alphaCutoff = BLEND_STANDIN_CUTOFF;
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

        const LLMaterialPtr params = te->getMaterialParams();
        const U8 alphaMode = params.notNull() ? params->getDiffuseAlphaMode() : U8(LLMaterial::DIFFUSE_ALPHA_MODE_BLEND);
        const bool textureAlpha = image && image->getComponents() == 4;
        if (alphaMode == LLMaterial::DIFFUSE_ALPHA_MODE_MASK)
        {
            flags |= asr::INSTANCE_MASKED;
            material.alphaCutoff = F32(params->getAlphaMaskCutoff()) / 255.f;
        }
        else if (material.color[3] < 1.f || (alphaMode == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND && textureAlpha))
        {
            flags |= asr::INSTANCE_MASKED;
            material.alphaCutoff = BLEND_STANDIN_CUTOFF;
        }

        F32 scaleS = 1.f;
        F32 scaleT = 1.f;
        F32 offsetS = 0.f;
        F32 offsetT = 0.f;
        te->getScale(&scaleS, &scaleT);
        te->getOffset(&offsetS, &offsetT);
        setUVTransform(material, cosf(te->getRotation()), sinf(te->getRotation()), scaleS, scaleT, offsetS, offsetT);

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
    }
    if (drawable == sShifted)
    {
        desc.objectToWorld[11] += sShift;
        desc.lodCenter[2] += sShift;
    }
    desc.lodSize = shape.lodBias.scaledVec(volume->getScale()).length();
}

// Why an object is not drawn yet, or null when it can be.
const char* whyLeftOut(LLDrawable* drawable, LLVOVolume* volume)
{
    if (drawable->isDead() || !volume->getVolume())
    {
        return "no volume";
    }
    if (volume->getAvatarAncestor() || volume->isHUDAttachment())
    {
        return "attached";
    }
    if (volume->isMesh() || drawable->isState(LLDrawable::RIGGED))
    {
        return "mesh";
    }
    if (volume->isSculpted())
    {
        return "sculpt";
    }
    return volume->isFlexible() ? "flexible" : nullptr;
}

LLVOVolume* volumeOf(LLDrawable* drawable)
{
    LLViewerObject* object = drawable && !drawable->isSpatialBridge() ? drawable->getVObj().get() : nullptr;
    return object && object->getPCode() == LL_PCODE_VOLUME ? static_cast<LLVOVolume*>(object) : nullptr;
}

void writeFace(ObjectFace& face, const ShapeFace& shapeFace, asr::InstanceDesc& desc)
{
    desc.geometry = face.hidden ? 0 : shapeFace.finest;
    desc.material = face.material->id;
    desc.flags = face.flags | (face.rank << asr::INSTANCE_RANK_SHIFT);
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
            const bool masked = (face.flags & asr::INSTANCE_MASKED) != 0;
            const bool hidden = !masked && covered;
            const U32 faceRank = masked && stack.members.size() > 1 ? llmin(++rank, asr::INSTANCE_RANK_MAX) : 0;
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
                    writeFace(object.faces[i], object.shape->faces[i], desc);
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
        auto found = sObjects.find(drawable);
        if (found != sObjects.end())
        {
            clearObject(drawable, found->second);
            sObjects.erase(found);
        }
        return;
    }

    WorldObject& object = sObjects[drawable];
    const LLVolumeParams& params = volume->getVolume()->getParams();
    if (object.shape && !(object.shape->params == params))
    {
        clearObject(drawable, object);
    }
    if (!object.shape)
    {
        object.shape = useShape(params);
    }
    if (!object.shape->ready)
    {
        object.shape->waiting.insert(drawable);
        return;
    }

    const Shape& shape = *object.shape;
    const size_t faces = llmin(size_t(volume->getNumTEs()), shape.faces.size());
    for (size_t i = faces; i < object.faces.size(); ++i)
    {
        clearFace(object.faces[i], drawable);
    }
    object.faces.resize(faces);

    asr::InstanceDesc desc;
    fillTransform(desc, drawable, volume, shape);
    for (size_t i = 0; i < faces; ++i)
    {
        ObjectFace& face = object.faces[i];
        asr::MaterialDesc material;
        LLViewerTexture* image = nullptr;
        U32 flags = 0;
        if (shape.faces[i].finest == 0 || !fillMaterial(material, flags, image, volume, S32(i), shape.faces[i].box))
        {
            clearFace(face, drawable);
            continue;
        }

        // The new one comes first, so a material that stays is not made again, and the
        // old one goes last, so no instance is left pointing at it.
        SharedMaterial* previous = face.material;
        face.material = useMaterial(material, image, drawable);
        face.flags = flags;
        writeFace(face, shape.faces[i], desc);
        if (previous)
        {
            releaseMaterial(previous, drawable);
        }
    }
    placeInStack(drawable, object, desc);
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
    asr::InstanceDesc desc;
    fillTransform(desc, drawable, volume, *object.shape);
    placeInStack(drawable, object, desc);
    for (size_t i = 0; i < object.faces.size(); ++i)
    {
        ObjectFace& face = object.faces[i];
        if (face.instance != NO_INSTANCE && !face.hidden)
        {
            writeFace(face, object.shape->faces[i], desc);
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

void applyFinishedShapes()
{
    std::vector<Shape*> finished;
    {
        std::lock_guard<std::mutex> lock(sJobMutex);
        finished.swap(sFinished);
    }
    for (Shape* shape : finished)
    {
        shape->ready = true;
        if (shape->users == 0)
        {
            forgetShape(shape);
            continue;
        }
        sChanged.insert(shape->waiting.begin(), shape->waiting.end());
        shape->waiting.clear();
    }
}

// Sharper versions of a texture arrive in a new slot, and an alpha channel can appear. A
// new slot only touches the materials on that texture. An alpha channel can change how
// its faces draw, so those go through applyObject() again.
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
            if (slot == 0 && ++sLostTextures <= 5)
            {
                LL_INFOS("ASRenderer") << "texture lost its image: " << texture.image->getID() << " on " << texture.users.size() << " objects, discard "
                                       << texture.image->getDiscardLevel() << ", size " << texture.image->getMaxVirtualSize() << ", bias "
                                       << LLViewerTexture::sDesiredDiscardBias << LL_ENDL;
            }
            texture.slot = slot;
            writeMaterials(texture);
        }
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
    asr::SceneCamera3D scene = asr::makeCameraFromAxes(camera->getOrigin().mV, camera->getAtAxis().mV, camera->getUpAxis().mV, camera->getView(),
                                                       camera->getAspect(), camera->getNear(), camera->getFar());

    // What calcLOD() does to distance and to the LOD factor, apart from its boost very
    // close up, which only ever chooses the finest LOD earlier.
    F32 factor = LLVOVolume::sLODFactor;
    static LLCachedControl<bool> ignore_fov_zoom(gSavedSettings, "IgnoreFOVZoomForLODs");
    if (!ignore_fov_zoom)
    {
        factor *= DEFAULT_FIELD_OF_VIEW / camera->getDefaultFOV();
    }
    scene.lodScale = (F_PI / 3.f) * LLVOVolume::sDistanceFactor / llmax(factor, 0.001f);
    ASRViewer::setCamera(scene);
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
            && (avatar->isSelf() || camera->sphereInFrustum(avatar->getPositionAgent(), AVATAR_RADIUS)))
        {
            avatar->mDrawable->setVisible(*camera);
        }
    }
}

void logCounts()
{
    if (std::chrono::steady_clock::now() - sLogged >= LOG_PERIOD)
    {
        sLogged = std::chrono::steady_clock::now();
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
                const LLVector3 at(desc.lodCenter);
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
            }
        }
        const size_t shown = llmin(waiting.size(), size_t(5));
        std::partial_sort(waiting.begin(), waiting.begin() + shown, waiting.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        LL_INFOS("ASRenderer") << "no image: " << missing << " missing, " << fetching << " fetching, " << idle << " idle" << LL_ENDL;
        const asr::MemoryBudget& memory = ASRViewer::device()->memoryBudget();
        LL_INFOS("ASRenderer") << "texture memory " << memory.used(asr::MemoryCategory::Textures) / (1024 * 1024) << " of "
                               << memory.limit(asr::MemoryCategory::Textures) / (1024 * 1024) << " MB" << LL_ENDL;

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
                               << " MB, shrunk so far " << shrunk << ", frozen " << LLViewerTexture::sFreezeImageUpdates << LL_ENDL;
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

        std::ostringstream reasons;
        for (const auto& reason : leftOut)
        {
            reasons << ", " << reason.second << " " << reason.first;
        }
        LL_INFOS("ASRenderer") << sObjects.size() << " objects, " << sShapes.size() << " shapes, " << sMaterials.size() << " materials, " << sTextures.size() << " textures, "
                               << stats.instances << " instances, " << stats.triangles << " triangles, " << stats.earlyClusters << " + " << stats.lateClusters
                               << " clusters drawn, " << stats.pendingGeometries << " geometries waiting, camera at " << LLViewerCamera::getInstance()->getOrigin()
                               << ", left out:" << reasons.str() << LL_ENDL;
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
    sObjects.clear();
    sLeftOut.clear();
    sMaterials.clear();
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

void ASRWorld::objectRemoved(LLDrawable* drawable)
{
    if (!sRunning)
    {
        return;
    }
    sChanged.erase(drawable);
    sMoved.erase(drawable);
    sLeftOut.erase(drawable);
    if (drawable == sShifted)
    {
        sShifted = nullptr;
    }
    auto found = sObjects.find(drawable);
    if (found != sObjects.end())
    {
        clearObject(drawable, found->second);
        sObjects.erase(found);
    }
}

void ASRWorld::worldShifted()
{
    if (sRunning)
    {
        for (const auto& object : sObjects)
        {
            sMoved.insert(object.first);
        }
    }
}

void ASRWorld::update()
{
    if (!sRunning)
    {
        return;
    }
    cullObjectCache();
    applyFinishedShapes();
    followTextures();
    updateLocalShift();
    for (LLDrawable* drawable : sChanged)
    {
        sMoved.erase(drawable); // applying it writes its transform too
        applyObject(drawable);
    }
    sChanged.clear();
    for (LLDrawable* drawable : sMoved)
    {
        moveObject(drawable);
    }
    sMoved.clear();
    publishCamera();
    logCounts();
}

#else // the new renderer is not in this build

void ASRWorld::start() {}
void ASRWorld::stop() {}
void ASRWorld::objectChanged(LLDrawable*) {}
void ASRWorld::objectMoved(LLDrawable*) {}
void ASRWorld::objectRemoved(LLDrawable*) {}
void ASRWorld::worldShifted() {}
void ASRWorld::update() {}

#endif
