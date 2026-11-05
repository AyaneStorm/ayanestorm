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

inline asr::GeometryData build(const Mesh& mesh)
{
    return asr::buildGeometry(mesh.vertices.data(), uint32_t(mesh.vertices.size()), mesh.indices.data(), uint32_t(mesh.indices.size()));
}

} // namespace asrtest

#ifdef ASR_HAS_DEVICE

#include "asrtexturequeue.h"

#include <algorithm>

namespace asrtest
{

class TestCity
{
public:
    TestCity(asr::SceneQueue& scene, asr::TextureQueue& textures) : mScene(scene)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            mBoxFaces[axis * 2] = scene.addGeometry(build(makeBoxFace(axis, 1.f)));
            mBoxFaces[axis * 2 + 1] = scene.addGeometry(build(makeBoxFace(axis, -1.f)));
        }
        mSphere = scene.addGeometry(build(makeSphere(24, 48)));
        mKnotMesh = makeKnot(180, 28);
        mKnot = scene.addGeometry(build(mKnotMesh));

        for (uint32_t i = 0; i < MATERIALS; ++i)
        {
            asr::MaterialDesc material;
            material.color[0] = 0.55f + 0.45f * std::sin(i * 1.3f);
            material.color[1] = 0.55f + 0.45f * std::sin(i * 2.1f + 1.f);
            material.color[2] = 0.55f + 0.45f * std::sin(i * 0.7f + 2.f);
            material.texture = checker(textures, i, i == 1 ? 4096 : 256);
            material.uvRow0[0] = 1.f + float(i % 3);
            material.uvRow1[1] = 1.f + float(i % 3);
            mMaterials[i] = scene.addMaterial(material);
        }

        // The ground.
        place(mBoxFaces[4], mMaterials[0], 0.f, 0.f, 0.f, EXTENT * 2.2f, EXTENT * 2.2f, 1.f, false);

        uint32_t random = 12345;
        const auto next = [&random]() { random = random * 1664525u + 1013904223u; return (random >> 16) / 65536.f; };
        const uint32_t side = uint32_t(std::sqrt(float(OBJECTS)));
        for (uint32_t i = 0; i < OBJECTS; ++i)
        {
            const float x = (float(i % side) / side - 0.5f) * EXTENT * 2.f + next() * 3.f;
            const float y = (float(i / side) / side - 0.5f) * EXTENT * 2.f + next() * 3.f;
            const uint32_t material = mMaterials[i % MATERIALS];
            const float kind = next();
            if (kind < 0.8f)
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
        const float angle = seconds * 0.12f;
        const float eye[3] = { std::cos(angle) * 120.f, std::sin(angle) * 120.f, 40.f + 20.f * std::sin(seconds * 0.3f) };
        const float target[3] = { std::cos(angle + 0.8f) * 30.f, std::sin(angle + 0.8f) * 30.f, 6.f };
        return asr::makeCamera(eye, target, 1.05f, float(width) / float(std::max(height, 1u)), 0.1f);
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
    static uint32_t checker(asr::TextureQueue& textures, uint32_t index, uint32_t SIZE)
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
                pixels[y * SIZE + x] = ((x / cell + y / cell) & 1) ? 0xFFFFFFFFu : 0xFFA0A0A0u;
            }
        }
        const uint32_t slot = textures.create(desc);
        textures.upload(slot, 0, 0, 0, SIZE, SIZE, pixels.data(), pixels.size() * sizeof(uint32_t));
        textures.generateMips(slot);
        return slot;
    }

    static asr::InstanceDesc instanceDesc(uint32_t geometry, uint32_t material, float x, float y, float z, float sx, float sy, float sz)
    {
        asr::InstanceDesc desc;
        desc.geometry = geometry;
        desc.material = material;
        const float rows[12] = { sx, 0.f, 0.f, x, 0.f, sy, 0.f, y, 0.f, 0.f, sz, z };
        std::copy(rows, rows + 12, desc.objectToWorld);
        return desc;
    }

    uint32_t place(uint32_t geometry, uint32_t material, float x, float y, float z, float sx, float sy, float sz, bool)
    {
        ++mInstances;
        return mScene.addInstance(instanceDesc(geometry, material, x, y, z, sx, sy, sz));
    }

    asr::SceneQueue& mScene;
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
