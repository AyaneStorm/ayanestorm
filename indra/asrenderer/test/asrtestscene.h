/**
 * asrtestscene.h
 * Made-up geometry and a made-up city for asrenderer_test. The city has the shape of a
 * busy region: many small prims with flat faces, some round objects, and detailed
 * meshes used many times. Some objects move, and some geometry is replaced, on the
 * main thread while the render thread draws.
 */
#pragma once

#include "asrscene.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace asrtest
{

struct Mesh
{
    std::vector<asr::SceneVertex> vertices;
    std::vector<uint16_t> indices;
};

constexpr float TWO_PI = 6.2831853f;

inline asr::SceneVertex vertexAt(float x, float y, float z, float nx, float ny, float nz, float u, float v)
{
    asr::SceneVertex vertex{};
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = z;
    vertex.normal[0] = nx;
    vertex.normal[1] = ny;
    vertex.normal[2] = nz;
    vertex.tangent[0] = 1.f;
    vertex.tangent[3] = 1.f;
    vertex.uv[0] = u;
    vertex.uv[1] = v;
    return vertex;
}

// Quads between rows of a grid of (columns + 1) by (rows + 1) vertices.
inline void addGrid(Mesh& mesh, uint32_t rows, uint32_t columns, uint16_t base)
{
    for (uint32_t row = 0; row < rows; ++row)
    {
        for (uint32_t column = 0; column < columns; ++column)
        {
            const uint16_t a = uint16_t(base + row * (columns + 1) + column);
            const uint16_t b = uint16_t(a + 1);
            const uint16_t c = uint16_t(a + columns + 1);
            const uint16_t d = uint16_t(c + 1);
            mesh.indices.insert(mesh.indices.end(), { a, c, b, b, c, d });
        }
    }
}

// Unit radius, outward facing, seen counter-clockwise from outside.
inline Mesh makeSphere(uint32_t rows, uint32_t columns)
{
    Mesh mesh;
    for (uint32_t row = 0; row <= rows; ++row)
    {
        const float theta = float(row) / rows * 3.1415927f;
        for (uint32_t column = 0; column <= columns; ++column)
        {
            const float phi = float(column) / columns * TWO_PI;
            const float x = std::sin(theta) * std::cos(phi);
            const float y = std::sin(theta) * std::sin(phi);
            const float z = std::cos(theta);
            mesh.vertices.push_back(vertexAt(x, y, z, x, y, z, float(column) / columns * 4.f, float(row) / rows * 2.f));
        }
    }
    addGrid(mesh, rows, columns, 0);
    return mesh;
}

// A ring wound around a torus: dense, and folded over itself, like detailed meshes.
inline Mesh makeKnot(uint32_t rows, uint32_t columns)
{
    Mesh mesh;
    const auto centre = [](float t, float out[3])
    {
        const float r = 0.62f + 0.24f * std::cos(3.f * t);
        out[0] = r * std::cos(2.f * t);
        out[1] = r * std::sin(2.f * t);
        out[2] = 0.24f * std::sin(3.f * t);
    };
    for (uint32_t row = 0; row <= rows; ++row)
    {
        const float t = float(row) / rows * TWO_PI;
        float p[3], q[3];
        centre(t, p);
        centre(t + 0.01f, q);
        float along[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
        const float size = std::sqrt(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
        for (float& value : along)
        {
            value /= size;
        }
        // Two axes around the tube.
        float side[3] = { -along[1], along[0], 0.f };
        const float sideSize = std::sqrt(side[0] * side[0] + side[1] * side[1]) + 1e-6f;
        side[0] /= sideSize;
        side[1] /= sideSize;
        const float up[3] = { along[1] * side[2] - along[2] * side[1], along[2] * side[0] - along[0] * side[2],
                              along[0] * side[1] - along[1] * side[0] };
        for (uint32_t column = 0; column <= columns; ++column)
        {
            const float angle = float(column) / columns * TWO_PI;
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            const float n[3] = { side[0] * c + up[0] * s, side[1] * c + up[1] * s, side[2] * c + up[2] * s };
            constexpr float TUBE = 0.11f;
            mesh.vertices.push_back(vertexAt(p[0] + n[0] * TUBE, p[1] + n[1] * TUBE, p[2] + n[2] * TUBE, n[0], n[1], n[2],
                                             float(row) / rows * 12.f, float(column) / columns));
        }
    }
    addGrid(mesh, rows, columns, 0);
    return mesh;
}

// One flat face of a unit box, as a volume face arrives: six of these make a cube.
inline Mesh makeBoxFace(int axis, float sign)
{
    Mesh mesh;
    const int u = (axis + 1) % 3;
    const int v = (axis + 2) % 3;
    for (int corner = 0; corner < 4; ++corner)
    {
        float position[3];
        float normal[3] = { 0.f, 0.f, 0.f };
        position[axis] = 0.5f * sign;
        position[u] = (corner & 1 ? 0.5f : -0.5f);
        position[v] = (corner & 2 ? 0.5f : -0.5f) * sign;
        normal[axis] = sign;
        mesh.vertices.push_back(vertexAt(position[0], position[1], position[2], normal[0], normal[1], normal[2], float(corner & 1), float(corner >> 1)));
    }
    mesh.indices = { 0, 1, 2, 2, 1, 3 };
    return mesh;
}

inline asr::GeometryData build(const Mesh& mesh, const asr::GeometryBox* box = nullptr)
{
    return asr::buildGeometry(mesh.vertices.data(), uint32_t(mesh.vertices.size()), mesh.indices.data(), uint32_t(mesh.indices.size()), box);
}

} // namespace asrtest

#ifdef ASR_HAS_DEVICE

#include "asrtexturequeue.h"
#include "asruilist.h"

#include <algorithm>

namespace asrtest
{

class TestCity
{
public:
    TestCity(asr::SceneQueue& scene, asr::TextureQueue& textures, bool street) : mScene(scene), mStreet(street)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            mBoxFaces[axis * 2] = scene.addGeometry(build(makeBoxFace(axis, 1.f)));
            mBoxFaces[axis * 2 + 1] = scene.addGeometry(build(makeBoxFace(axis, -1.f)));
        }

        // Three LODs each, coarsest first, all packed in one box.
        const asr::GeometryBox unitBox{ { 0.f, 0.f, 0.f }, 1.f };
        uint32_t coarser = scene.addGeometry(build(makeSphere(6, 12), &unitBox));
        coarser = scene.addGeometry(build(makeSphere(12, 24), &unitBox), coarser, 40.f);
        mSphere = scene.addGeometry(build(makeSphere(24, 48), &unitBox), coarser, 14.f);
        mKnotMesh = makeKnot(180, 28);
        coarser = scene.addGeometry(build(makeKnot(45, 8), &unitBox));
        coarser = scene.addGeometry(build(makeKnot(90, 14), &unitBox), coarser, 40.f);
        mKnot = scene.addGeometry(build(mKnotMesh, &unitBox), coarser, 14.f);

        asr::MaterialDesc fence;
        fence.texture = checker(textures, 0, 256, 0x00FFFFFFu);
        mFence = scene.addMaterial(fence);

        for (uint32_t i = 0; i < MATERIALS; ++i)
        {
            asr::MaterialDesc material;
            material.color[0] = 0.55f + 0.45f * std::sin(i * 1.3f);
            material.color[1] = 0.55f + 0.45f * std::sin(i * 2.1f + 1.f);
            material.color[2] = 0.55f + 0.45f * std::sin(i * 0.7f + 2.f);
            material.texture = checker(textures, i, i == 1 ? 4096 : 256, 0xFFA0A0A0u);
            if (i == 0)
            {
                mGroundTexture = material.texture;
            }
            material.uvRow0[0] = 1.f + float(i % 3);
            material.uvRow1[1] = 1.f + float(i % 3);
            mMaterials[i] = scene.addMaterial(material);
        }

        // The ground.
        place(mBoxFaces[4], mMaterials[0], 0.f, 0.f, 0.f, EXTENT * 2.2f, EXTENT * 2.2f, 1.f, false);
        addHills(scene, textures);

        uint32_t random = 12345;
        const auto next = [&random]() { random = random * 1664525u + 1013904223u; return (random >> 16) / 65536.f; };
        const uint32_t side = uint32_t(std::sqrt(float(OBJECTS)));
        for (uint32_t i = 0; i < OBJECTS; ++i)
        {
            const float x = (float(i % side) / side - 0.5f) * EXTENT * 2.f + next() * 3.f;
            const float y = (float(i / side) / side - 0.5f) * EXTENT * 2.f + next() * 3.f;
            const uint32_t material = mMaterials[i % MATERIALS];
            const float kind = next();
            if (kind < 0.04f)
            {
                // A standing sheet: seen from both sides, and every other one has holes.
                const bool holes = kind < 0.02f;
                const float width = 2.f + next() * 6.f;
                const float height = 2.f + next() * 6.f;
                place(mBoxFaces[0], holes ? mFence : material, x, y, height * 0.5f + 0.5f, 1.f, width, height, false,
                      asr::INSTANCE_DOUBLE_SIDED | (holes ? asr::INSTANCE_MASKED : 0));
            }
            else if (kind < 0.8f)
            {
                const float height = 0.5f + next() * next() * 18.f;
                const float width = 0.5f + next() * 3.f;
                const float depth = 0.5f + next() * 3.f;
                for (uint32_t face = 0; face < 6; ++face)
                {
                    place(mBoxFaces[face], material, x, y, height * 0.5f + 0.5f, width, depth, height, false);
                }
            }
            else if (kind < 0.92f)
            {
                const float size = 0.4f + next() * 2.f;
                const bool moves = (i % 4) == 0;
                const uint32_t instance = place(mSphere, material, x, y, size + 0.5f, size, size, size, moves);
                if (moves)
                {
                    mMoving.push_back({ instance, mSphere, material, x, y, size + 0.5f, size, next() * TWO_PI });
                }
            }
            else
            {
                const float size = 1.f + next() * 3.f;
                place(mKnot, material, x, y, size + 0.6f, size, size, size, false);
            }
            ++mObjects;
        }
        // A short-lived mesh, replaced now and then, so memory is reused under load.
        mReplaced = 0;
        mSwapGeometry = scene.addGeometry(build(mKnotMesh));
        mSwapInstance = place(mSwapGeometry, mMaterials[1], 0.f, 0.f, 30.f, 12.f, 12.f, 12.f, false);
    }

    asr::SceneCamera3D camera(float seconds, uint32_t width, uint32_t height) const
    {
        const float aspect = float(width) / float(std::max(height, 1u));
        if (mStreet)
        {
            // Walking between the buildings, where nearly everything hides something.
            const float walk = seconds * 0.05f;
            const float eye[3] = { std::cos(walk) * 70.f, std::sin(walk) * 70.f, 1.8f };
            const float target[3] = { std::cos(walk + 0.2f) * 70.f, std::sin(walk + 0.2f) * 70.f, 2.5f };
            return asr::makeCamera(eye, target, 1.05f, aspect, 0.1f);
        }
        const float angle = seconds * 0.12f;
        const float eye[3] = { std::cos(angle) * 120.f, std::sin(angle) * 120.f, 40.f + 20.f * std::sin(seconds * 0.3f) };
        const float target[3] = { std::cos(angle + 0.8f) * 30.f, std::sin(angle + 0.8f) * 30.f, 6.f };
        return asr::makeCamera(eye, target, 1.05f, aspect, 0.1f);
    }

    void update(float seconds)
    {
        if (seconds - mLastMove >= 1.f / 45.f)
        {
            mLastMove = seconds;
            for (const Moving& object : mMoving)
            {
                asr::InstanceDesc desc = instanceDesc(object.geometry, object.material, object.x + 4.f * std::sin(seconds + object.phase),
                                                      object.y + 4.f * std::cos(seconds + object.phase), object.z, object.size, object.size, object.size);
                mScene.setInstance(object.instance, desc);
            }
            mMoved += uint32_t(mMoving.size());
        }
        if (seconds - mLastSwap >= 0.5f)
        {
            mLastSwap = seconds;
            const uint32_t fresh = mScene.addGeometry(build(mKnotMesh));
            mScene.setInstance(mSwapInstance, instanceDesc(fresh, mMaterials[1], 0.f, 0.f, 30.f, 12.f, 12.f, 12.f));
            mScene.removeGeometry(mSwapGeometry);
            mSwapGeometry = fresh;
            ++mReplaced;
        }
    }

    uint32_t objects() const { return mObjects; }
    uint32_t instances() const { return mInstances; }
    uint32_t moved() const { return mMoved; }
    uint32_t replaced() const { return mReplaced; }
    uint32_t groundTexture() const { return mGroundTexture; } // a slot

private:
    static constexpr uint32_t OBJECTS = 16000;
    static constexpr uint32_t MATERIALS = 32;
    static constexpr float EXTENT = 128.f;

    struct Moving
    {
        uint32_t instance;
        uint32_t geometry;
        uint32_t material;
        float x, y, z, size, phase;
    };

    // One texture is large enough to span several frames of uploads.
    static uint32_t checker(asr::TextureQueue& textures, uint32_t index, uint32_t SIZE, uint32_t dark)
    {
        asr::TextureDesc desc;
        desc.width = SIZE;
        desc.height = SIZE;
        desc.mipLevels = 1;
        while ((SIZE >> desc.mipLevels) != 0)
        {
            ++desc.mipLevels;
        }
        const uint32_t cell = 8u << (index % 4);
        std::vector<uint32_t> pixels(SIZE * SIZE);
        for (uint32_t y = 0; y < SIZE; ++y)
        {
            for (uint32_t x = 0; x < SIZE; ++x)
            {
                pixels[y * SIZE + x] = ((x / cell + y / cell) & 1) ? 0xFFFFFFFFu : dark;
            }
        }
        const uint32_t slot = textures.create(desc);
        textures.upload(slot, 0, 0, 0, SIZE, SIZE, pixels.data(), pixels.size() * sizeof(uint32_t));
        textures.generateMips(slot);
        return slot;
    }

    static float hillHeight(float x, float y)
    {
        return 12.f + 10.f * std::sin(x * 0.021f) * std::cos(y * 0.017f) + 4.f * std::sin(x * 0.11f + y * 0.07f);
    }

    // A region of ground east of the city, as the viewer's terrain arrives: 4 by 4 patches,
    // each with four LODs that share their edges, packed in one box for the whole region.
    // The west half blends four textures, the east half four materials in three planes.
    void addHills(asr::SceneQueue& scene, asr::TextureQueue& textures)
    {
        constexpr uint32_t PATCHES = 4;
        constexpr uint32_t SIZE = 16;
        constexpr float SPACING = 4.f;
        constexpr float WIDTH = PATCHES * SIZE * SPACING;
        const float origin[3] = { EXTENT * 1.15f, -WIDTH * 0.5f, 0.f };

        asr::TerrainDesc classic;
        asr::TerrainDesc materials;
        materials.kind = asr::TERRAIN_MATERIALS;
        materials.planes = 3;
        for (uint32_t i = 0; i < asr::TERRAIN_LAYERS; ++i)
        {
            const uint32_t bits = asr::uiTextureBits(checker(textures, i + 1, 128, 0xFF000000u | (0x3F << (i * 6 % 24))), asr::UISampler::LinearWrap);
            classic.layers[i].texture = bits;
            classic.layers[i].uvRow0[0] = 1.f / 12.f; // RenderTerrainScale
            classic.layers[i].uvRow1[1] = 1.f / 12.f;
            materials.layers[i].texture = bits;
            materials.layers[i].uvRow0[0] = 1.f / 8.f; // RenderTerrainPBRScale, with no texture transform
            materials.layers[i].uvRow1[1] = 1.f / 8.f;
            materials.layers[i].color[i % 3] = 0.6f;
        }
        classic.ramp = materials.ramp = asr::uiTextureBits(ramp(textures), asr::UISampler::LinearClamp);
        const uint32_t terrains[2] = { scene.addTerrain(classic), scene.addTerrain(materials) };

        const asr::GeometryBox box{ { WIDTH * 0.5f, WIDTH * 0.5f, 20.f }, WIDTH * 0.5f };
        uint32_t terrainMaterials[2];
        for (uint32_t i = 0; i < 2; ++i)
        {
            asr::MaterialDesc material;
            material.terrain = terrains[i];
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                material.planarScale[axis] = box.scale;
                material.planarOffset[axis] = box.center[axis];
            }
            terrainMaterials[i] = scene.addMaterial(material);
        }

        constexpr uint32_t POINTS = SIZE + 1;
        std::vector<float> heights(POINTS * POINTS);
        std::vector<float> normals(POINTS * POINTS * 3);
        std::vector<float> blends(POINTS * POINTS * 2);
        std::vector<asr::SceneVertex> vertices;
        std::vector<uint16_t> indices;
        for (uint32_t py = 0; py < PATCHES; ++py)
        {
            for (uint32_t px = 0; px < PATCHES; ++px)
            {
                asr::TerrainGrid grid;
                grid.size = SIZE;
                grid.origin[0] = px * SIZE * SPACING;
                grid.origin[1] = py * SIZE * SPACING;
                grid.spacing = SPACING;
                for (uint32_t y = 0; y < POINTS; ++y)
                {
                    for (uint32_t x = 0; x < POINTS; ++x)
                    {
                        const uint32_t at = y * POINTS + x;
                        const float rx = grid.origin[0] + x * SPACING;
                        const float ry = grid.origin[1] + y * SPACING;
                        heights[at] = hillHeight(rx, ry);
                        float n[3] = { hillHeight(rx - 1.f, ry) - hillHeight(rx + 1.f, ry), hillHeight(rx, ry - 1.f) - hillHeight(rx, ry + 1.f), 2.f };
                        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        for (uint32_t axis = 0; axis < 3; ++axis)
                        {
                            normals[at * 3 + axis] = n[axis] / length;
                        }
                        blends[at * 2] = std::min(std::max((heights[at] - 2.f) / 24.f * 3.f + 0.4f * std::sin(rx * 0.3f), 0.f), 3.f);
                        blends[at * 2 + 1] = 0.5f + 0.5f * std::sin(rx * 0.13f + ry * 0.29f);
                    }
                }
                grid.heights = heights.data();
                grid.normals = normals.data();
                grid.blends = blends.data();

                uint32_t coarser = 0;
                const float coarserAt[4] = { 0.f, 3.f, 6.f, 12.f };
                for (int lod = 3; lod >= 0; --lod)
                {
                    asr::buildTerrainLod(grid, 1u << lod, vertices, indices);
                    coarser = scene.addGeometry(asr::buildGeometry(vertices.data(), uint32_t(vertices.size()), indices.data(), uint32_t(indices.size()), &box), coarser,
                                                coarserAt[lod + 1 < 4 ? lod + 1 : 3]);
                }

                asr::InstanceDesc desc = instanceDesc(coarser, terrainMaterials[px < PATCHES / 2 ? 0 : 1], origin[0], origin[1], origin[2], 1.f, 1.f, 1.f);
                const float half = SIZE * SPACING * 0.5f;
                desc.lodCenter[0] = origin[0] + grid.origin[0] + half;
                desc.lodCenter[1] = origin[1] + grid.origin[1] + half;
                desc.lodCenter[2] = hillHeight(grid.origin[0] + half, grid.origin[1] + half);
                desc.lodSize = half * 1.4142f;
                ++mInstances;
                scene.addInstance(desc);
            }
        }
    }

    // Like the viewer's alpha ramp: full at 0, gone at 1, along u.
    static uint32_t ramp(asr::TextureQueue& textures)
    {
        constexpr uint32_t WIDTH = 64;
        constexpr uint32_t HEIGHT = 4;
        asr::TextureDesc desc;
        desc.width = WIDTH;
        desc.height = HEIGHT;
        desc.mipLevels = 1;
        std::vector<uint32_t> pixels(WIDTH * HEIGHT);
        for (uint32_t y = 0; y < HEIGHT; ++y)
        {
            for (uint32_t x = 0; x < WIDTH; ++x)
            {
                const uint32_t alpha = 255u - x * 255u / (WIDTH - 1);
                pixels[y * WIDTH + x] = (alpha << 24) | 0x00FFFFFFu;
            }
        }
        const uint32_t slot = textures.create(desc);
        textures.upload(slot, 0, 0, 0, WIDTH, HEIGHT, pixels.data(), pixels.size() * sizeof(uint32_t));
        return slot;
    }

    static asr::InstanceDesc instanceDesc(uint32_t geometry, uint32_t material, float x, float y, float z, float sx, float sy, float sz, uint32_t flags = 0)
    {
        asr::InstanceDesc desc;
        desc.geometry = geometry;
        desc.material = material;
        desc.flags = flags;
        const float rows[12] = { sx, 0.f, 0.f, x, 0.f, sy, 0.f, y, 0.f, 0.f, sz, z };
        std::copy(rows, rows + 12, desc.objectToWorld);
        return desc;
    }

    uint32_t place(uint32_t geometry, uint32_t material, float x, float y, float z, float sx, float sy, float sz, bool, uint32_t flags = 0)
    {
        ++mInstances;
        return mScene.addInstance(instanceDesc(geometry, material, x, y, z, sx, sy, sz, flags));
    }

    asr::SceneQueue& mScene;
    bool mStreet;
    uint32_t mFence = 0;
    uint32_t mGroundTexture = 0;
    uint32_t mBoxFaces[6]{};
    uint32_t mSphere = 0;
    uint32_t mKnot = 0;
    Mesh mKnotMesh;
    uint32_t mMaterials[MATERIALS]{};
    std::vector<Moving> mMoving;
    uint32_t mSwapGeometry = 0;
    uint32_t mSwapInstance = 0;
    uint32_t mObjects = 0;
    uint32_t mInstances = 0;
    uint32_t mMoved = 0;
    uint32_t mReplaced = 0;
    float mLastMove = 0.f;
    float mLastSwap = 0.f;
};

} // namespace asrtest

#endif // ASR_HAS_DEVICE
