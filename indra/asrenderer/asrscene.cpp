/**
 * asrscene.cpp
 * See asrscene.h.
 */
#include "asrscene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <utility>

namespace asr
{

namespace
{

struct Vec3
{
    float x, y, z;
};

Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 operator*(const Vec3& a, float b) { return { a.x * b, a.y * b, a.z * b }; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }

Vec3 normalized(const Vec3& a)
{
    const float size = length(a);
    return size > 0.f ? a * (1.f / size) : Vec3{ 0.f, 0.f, 1.f };
}

Vec3 positionOf(const SceneVertex& vertex)
{
    return { vertex.position[0], vertex.position[1], vertex.position[2] };
}

int16_t snorm16(float value)
{
    return int16_t(std::lround(std::clamp(value, -1.f, 1.f) * 32767.f));
}

void octahedral(const float* n, int16_t out[2])
{
    const float sum = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    float x = sum > 0.f ? n[0] / sum : 0.f;
    float y = sum > 0.f ? n[1] / sum : 0.f;
    if (n[2] < 0.f)
    {
        const float wrappedX = (1.f - std::fabs(y)) * (x >= 0.f ? 1.f : -1.f);
        y = (1.f - std::fabs(x)) * (y >= 0.f ? 1.f : -1.f);
        x = wrappedX;
    }
    out[0] = snorm16(x);
    out[1] = snorm16(y);
}

uint32_t coneBits(const Vec3& axis, float cutoff)
{
    // Rounded so the packed cone never claims more than the real one.
    const auto byteOf = [](float value) { return uint32_t(uint8_t(int8_t(std::clamp(std::lround(value * 127.f), -127l, 127l)))); };
    // One step per axis of rounding error in the packed axis, plus the cutoff's own.
    const float packedCutoff = std::min(1.f, cutoff + 4.f / 127.f);
    return byteOf(axis.x) | byteOf(axis.y) << 8 | byteOf(axis.z) << 16 | byteOf(packedCutoff) << 24;
}

// The narrowest cone around the average of these unit normals that holds them all.
uint32_t normalCone(const std::vector<Vec3>& normals)
{
    Vec3 sum{ 0.f, 0.f, 0.f };
    for (const Vec3& normal : normals)
    {
        sum = sum + normal;
    }
    const Vec3 axis = normalized(sum);
    float cutoff = 1.f;
    for (const Vec3& normal : normals)
    {
        cutoff = std::min(cutoff, dot(normal, axis));
    }
    // Cutoff here is the sine of how far normals stray past the plane: 1 turns the test off.
    const float sine = cutoff <= 0.f || normals.empty() ? 1.f : std::sqrt(std::max(1.f - cutoff * cutoff, 0.f));
    return coneBits(axis, sine);
}

// Spreads the low 10 bits of value so two zero bits follow each one.
uint32_t spreadBits(uint32_t value)
{
    value &= 0x3FFu;
    value = (value | (value << 16)) & 0x030000FFu;
    value = (value | (value << 8)) & 0x0300F00Fu;
    value = (value | (value << 4)) & 0x030C30C3u;
    value = (value | (value << 2)) & 0x09249249u;
    return value;
}

// Where a packed position falls on a curve that visits nearby places one after another.
uint32_t mortonCode(const float center[3])
{
    const auto cell = [](float value) { return uint32_t(std::clamp((value + 1.f) * 512.f, 0.f, 1023.f)); };
    return spreadBits(cell(center[0])) | spreadBits(cell(center[1])) << 1 | spreadBits(cell(center[2])) << 2;
}

// Triangles grow out from a seed through shared vertices, nearest first, and turning
// away from the cluster's facing costs extra. So a cluster is compact and tends to face
// one way, which is what lets the GPU reject it whole.
class Clusterizer
{
public:
    Clusterizer(const SceneVertex* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t triangleCount)
        : mVertices(vertices), mIndices(indices), mTriangleCount(triangleCount)
    {
        mFirst.assign(size_t(vertexCount) + 1, 0);
        for (uint32_t i = 0; i < triangleCount * 3; ++i)
        {
            ++mFirst[size_t(indices[i]) + 1];
        }
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            mFirst[size_t(i) + 1] += mFirst[i];
        }
        mTriangles.resize(size_t(triangleCount) * 3);
        std::vector<uint32_t> next(mFirst.begin(), mFirst.end() - 1);
        for (uint32_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                mTriangles[next[indices[triangle * 3 + corner]]++] = triangle;
            }
        }
        mUsed.assign(triangleCount, 0);
        mQueued.assign(triangleCount, 0);
    }

    // The next cluster's triangles. False when none remain.
    bool next(std::vector<uint32_t>& cluster)
    {
        cluster.clear();
        while (mSeed < mTriangleCount && mUsed[mSeed])
        {
            ++mSeed;
        }
        if (mSeed == mTriangleCount)
        {
            return false;
        }

        ++mStamp;
        mFrontier.clear();
        Vec3 center{ 0.f, 0.f, 0.f };
        Vec3 facing{ 0.f, 0.f, 0.f };
        uint32_t chosen = mSeed;
        while (true)
        {
            mUsed[chosen] = 1;
            cluster.push_back(chosen);
            const Vec3 centre = centroid(chosen);
            center = center + (centre - center) * (1.f / float(cluster.size()));
            facing = facing + normal(chosen);
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                const uint32_t vertex = mIndices[chosen * 3 + corner];
                for (uint32_t i = mFirst[vertex]; i < mFirst[size_t(vertex) + 1]; ++i)
                {
                    const uint32_t neighbour = mTriangles[i];
                    if (!mUsed[neighbour] && mQueued[neighbour] != mStamp)
                    {
                        mQueued[neighbour] = mStamp;
                        mFrontier.push_back(neighbour);
                    }
                }
            }
            if (cluster.size() == CLUSTER_MAX_TRIANGLES)
            {
                return true;
            }

            float best = std::numeric_limits<float>::max();
            size_t bestAt = mFrontier.size();
            const Vec3 facingAxis = normalized(facing);
            for (size_t i = 0; i < mFrontier.size(); ++i)
            {
                const uint32_t candidate = mFrontier[i];
                const Vec3 offset = centroid(candidate) - center;
                const float turn = 1.5f - dot(normal(candidate), facingAxis);
                const float score = dot(offset, offset) * turn;
                if (score < best)
                {
                    best = score;
                    bestAt = i;
                }
            }
            if (bestAt == mFrontier.size())
            {
                return true; // this piece of the surface is used up
            }
            chosen = mFrontier[bestAt];
            mFrontier[bestAt] = mFrontier.back();
            mFrontier.pop_back();
        }
    }

private:
    Vec3 corner(uint32_t triangle, uint32_t index) const
    {
        return positionOf(mVertices[mIndices[triangle * 3 + index]]);
    }

    Vec3 centroid(uint32_t triangle) const
    {
        return (corner(triangle, 0) + corner(triangle, 1) + corner(triangle, 2)) * (1.f / 3.f);
    }

    Vec3 normal(uint32_t triangle) const
    {
        const Vec3 a = corner(triangle, 0);
        return normalized(cross(corner(triangle, 1) - a, corner(triangle, 2) - a));
    }

    const SceneVertex* mVertices;
    const uint16_t* mIndices;
    uint32_t mTriangleCount;
    std::vector<uint32_t> mFirst;     // where each vertex's triangles start in mTriangles
    std::vector<uint32_t> mTriangles;
    std::vector<uint8_t> mUsed;
    std::vector<uint32_t> mQueued;    // the stamp of the cluster that queued it
    std::vector<uint32_t> mFrontier;
    uint32_t mSeed = 0;
    uint32_t mStamp = 0;
};

template <class T>
void appendBytes(std::vector<uint8_t>& blob, const std::vector<T>& values)
{
    const size_t start = blob.size();
    blob.resize(start + values.size() * sizeof(T));
    memcpy(blob.data() + start, values.data(), values.size() * sizeof(T));
}

} // namespace

GeometryData buildGeometry(const SceneVertex* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount, const GeometryBox* box,
                           const SkinWeights* weights)
{
    GeometryData geometry;
    const uint32_t triangleCount = indexCount / 3;
    if (vertexCount == 0 || triangleCount == 0)
    {
        return geometry;
    }

    // One scale for all three axes, because cluster bounds and normal cones must stay
    // true in packed space. A flat face just leaves most of one axis unused.
    Vec3 low = positionOf(vertices[0]);
    Vec3 high = low;
    for (uint32_t i = 1; i < vertexCount; ++i)
    {
        const Vec3 p = positionOf(vertices[i]);
        low = { std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z) };
        high = { std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z) };
    }
    const Vec3 extent = (high - low) * 0.5f;
    const Vec3 center = box ? Vec3{ box->center[0], box->center[1], box->center[2] } : (low + high) * 0.5f;
    const float scale = box ? box->scale : std::max({ extent.x, extent.y, extent.z, 1e-6f });
    geometry.center[0] = center.x;
    geometry.center[1] = center.y;
    geometry.center[2] = center.z;
    geometry.scale = scale;
    const auto packed = [&](uint32_t vertex) { return (positionOf(vertices[vertex]) - center) * (1.f / scale); };

    std::vector<uint16_t> ordered;
    ordered.reserve(size_t(indexCount) + triangleCount / CLUSTER_MAX_TRIANGLES + 1);
    Clusterizer clusterizer(vertices, vertexCount, indices, triangleCount);
    std::vector<uint32_t> triangles;
    std::vector<std::vector<Vec3>> clusterNormals; // kept for the groups' cones
    while (clusterizer.next(triangles))
    {
        ClusterData cluster{};
        cluster.triangles = uint32_t(triangles.size());
        cluster.indexOffset = uint32_t(ordered.size() * sizeof(uint16_t));

        Vec3 boundLow{ 2.f, 2.f, 2.f };
        Vec3 boundHigh{ -2.f, -2.f, -2.f };
        for (uint32_t triangle : triangles)
        {
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                const uint16_t vertex = indices[triangle * 3 + corner];
                ordered.push_back(vertex);
                const Vec3 p = packed(vertex);
                boundLow = { std::min(boundLow.x, p.x), std::min(boundLow.y, p.y), std::min(boundLow.z, p.z) };
                boundHigh = { std::max(boundHigh.x, p.x), std::max(boundHigh.y, p.y), std::max(boundHigh.z, p.z) };
            }
        }
        if (weights)
        {
            // After the indices, which draws never read past: the joints that move the
            // cluster, so the skin pass bounds it from them alone. First their count, then
            // the most a vertex's weights sum strays from one, in 4/65535ths (rounded up, so
            // any sum of four weights fits).
            bool used[256] = {};
            uint32_t stray = 0;
            for (uint32_t triangle : triangles)
            {
                for (uint32_t corner = 0; corner < 3; ++corner)
                {
                    const SkinWeights& w = weights[indices[triangle * 3 + corner]];
                    int32_t sum = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        sum += w.weights[k];
                        used[w.joints[k]] = used[w.joints[k]] || w.weights[k] != 0;
                    }
                    stray = std::max(stray, uint32_t(std::abs(sum - 65535)));
                }
            }
            const size_t countAt = ordered.size();
            ordered.push_back(0);
            ordered.push_back(uint16_t((stray + 3) / 4));
            for (uint32_t joint = 0; joint < 256; ++joint)
            {
                if (used[joint])
                {
                    ordered.push_back(uint16_t(joint));
                    ++ordered[countAt];
                }
            }
        }
        if (ordered.size() % 2 != 0)
        {
            ordered.push_back(0); // the next cluster starts on four bytes, which shaders read in
        }

        const Vec3 middle = (boundLow + boundHigh) * 0.5f;
        float radius = 0.f;
        std::vector<Vec3> normals;
        normals.reserve(triangles.size());
        for (uint32_t triangle : triangles)
        {
            const Vec3 a = packed(indices[triangle * 3]);
            const Vec3 b = packed(indices[triangle * 3 + 1]);
            const Vec3 c = packed(indices[triangle * 3 + 2]);
            radius = std::max({ radius, length(a - middle), length(b - middle), length(c - middle) });
            const Vec3 facing = cross(b - a, c - a);
            if (dot(facing, facing) > 0.f)
            {
                normals.push_back(normalized(facing));
            }
        }
        // Packing moves vertices a little, so the sphere gets a margin of that size.
        radius += 2.f / 32767.f;

        cluster.center[0] = middle.x;
        cluster.center[1] = middle.y;
        cluster.center[2] = middle.z;
        cluster.radius = radius;
        cluster.cone = normalCone(normals);
        geometry.clusters.push_back(cluster);
        clusterNormals.push_back(std::move(normals));
    }

    // Groups of clusters that lie near each other: clusters in curve order, cut in runs.
    const size_t clusterCount = geometry.clusters.size();
    if (clusterCount > CLUSTERS_WITHOUT_GROUPS)
    {
        std::vector<uint32_t> order(clusterCount);
        std::vector<uint32_t> codes(clusterCount);
        for (uint32_t i = 0; i < clusterCount; ++i)
        {
            order[i] = i;
            codes[i] = mortonCode(geometry.clusters[i].center);
        }
        std::stable_sort(order.begin(), order.end(), [&codes](uint32_t a, uint32_t b) { return codes[a] < codes[b]; });
        std::vector<ClusterData> sorted(clusterCount);
        std::vector<std::vector<Vec3>> sortedNormals(clusterCount);
        for (size_t i = 0; i < clusterCount; ++i)
        {
            sorted[i] = geometry.clusters[order[i]];
            sortedNormals[i] = std::move(clusterNormals[order[i]]);
        }
        geometry.clusters = std::move(sorted);

        std::vector<Vec3> groupNormals;
        for (uint32_t first = 0; first < clusterCount; first += CLUSTER_GROUP_SIZE)
        {
            const uint32_t count = uint32_t(std::min<size_t>(CLUSTER_GROUP_SIZE, clusterCount - first));
            Vec3 low{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
            Vec3 high = low * -1.f;
            groupNormals.clear();
            for (uint32_t i = first; i < first + count; ++i)
            {
                const ClusterData& cluster = geometry.clusters[i];
                const Vec3 c{ cluster.center[0], cluster.center[1], cluster.center[2] };
                const Vec3 r{ cluster.radius, cluster.radius, cluster.radius };
                const Vec3 lowest = c - r;
                const Vec3 highest = c + r;
                low = { std::min(low.x, lowest.x), std::min(low.y, lowest.y), std::min(low.z, lowest.z) };
                high = { std::max(high.x, highest.x), std::max(high.y, highest.y), std::max(high.z, highest.z) };
                groupNormals.insert(groupNormals.end(), sortedNormals[i].begin(), sortedNormals[i].end());
            }
            const Vec3 middle = (low + high) * 0.5f;
            float radius = 0.f;
            for (uint32_t i = first; i < first + count; ++i)
            {
                const ClusterData& cluster = geometry.clusters[i];
                radius = std::max(radius, length(Vec3{ cluster.center[0], cluster.center[1], cluster.center[2] } - middle) + cluster.radius);
            }

            ClusterGroupData group{};
            group.center[0] = middle.x;
            group.center[1] = middle.y;
            group.center[2] = middle.z;
            group.radius = radius;
            group.cone = normalCone(groupNormals);
            group.firstCluster = first;
            group.clusterCount = count;
            geometry.groups.push_back(group);
        }
    }

    std::vector<PackedPosition> positions(vertexCount);
    std::vector<PackedAttributes> attributes(vertexCount);
    float radius = 0.f;
    for (uint32_t i = 0; i < vertexCount; ++i)
    {
        const Vec3 p = packed(i);
        radius = std::max(radius, length(p));
        positions[i] = { { snorm16(p.x), snorm16(p.y), snorm16(p.z) }, 0 };

        PackedAttributes& out = attributes[i];
        octahedral(vertices[i].normal, out.normal);
        octahedral(vertices[i].tangent, out.tangent);
        out.tangent[1] = int16_t((out.tangent[1] & ~1) | (vertices[i].tangent[3] < 0.f ? 1 : 0));
        out.uv[0] = vertices[i].uv[0];
        out.uv[1] = vertices[i].uv[1];
    }
    geometry.radius = radius + 2.f / 32767.f;

    appendBytes(geometry.blob, ordered);
    geometry.blob.resize((geometry.blob.size() + 15) & ~size_t(15));
    geometry.positionsOffset = uint32_t(geometry.blob.size());
    if (weights)
    {
        // Skinned positions stay full floats. Layered meshes (lipstick, eyeliner) sit a few
        // microns above the skin, the snorm grid of a large object is coarser than that, and
        // the layers would fight in depth. See packedPosition() in asrSceneCommon.glsl.
        std::vector<FloatPosition> exact(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            const Vec3 p = packed(i);
            exact[i] = { { p.x, p.y, p.z }, 1.f };
        }
        appendBytes(geometry.blob, exact);
    }
    else
    {
        appendBytes(geometry.blob, positions);
    }
    geometry.attributesOffset = uint32_t(geometry.blob.size());
    appendBytes(geometry.blob, attributes);
    geometry.vertexCount = vertexCount;
    if (weights)
    {
        // Right after the attributes, with no padding: shaders find them from the cluster's
        // attributes address and vertex count: see vertexCountOf() in asrSceneCommon.glsl.
        const size_t start = geometry.blob.size();
        geometry.blob.resize(start + size_t(vertexCount) * sizeof(SkinWeights));
        memcpy(geometry.blob.data() + start, weights, size_t(vertexCount) * sizeof(SkinWeights));
        geometry.skinned = true;

        // Each used joint's sphere: around the middle of its vertices' bounds, then as wide
        // as the farthest of them.
        constexpr uint32_t JOINTS = 256;
        std::vector<Vec3> jointLow(JOINTS, Vec3{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() });
        std::vector<Vec3> jointHigh(JOINTS, Vec3{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() });
        const auto eachUse = [&](auto&& use)
        {
            for (uint32_t i = 0; i < vertexCount; ++i)
            {
                for (int k = 0; k < 4; ++k)
                {
                    if (weights[i].weights[k] != 0)
                    {
                        use(weights[i].joints[k], positionOf(vertices[i]));
                    }
                }
            }
        };
        eachUse([&](uint32_t joint, const Vec3& p)
        {
            Vec3& l = jointLow[joint];
            Vec3& h = jointHigh[joint];
            l = { std::min(l.x, p.x), std::min(l.y, p.y), std::min(l.z, p.z) };
            h = { std::max(h.x, p.x), std::max(h.y, p.y), std::max(h.z, p.z) };
        });
        std::vector<float> jointRadius(JOINTS, 0.f);
        eachUse([&](uint32_t joint, const Vec3& p)
        {
            jointRadius[joint] = std::max(jointRadius[joint], length(p - (jointLow[joint] + jointHigh[joint]) * 0.5f));
        });
        for (uint32_t joint = 0; joint < JOINTS; ++joint)
        {
            if (jointLow[joint].x <= jointHigh[joint].x)
            {
                const Vec3 middle = (jointLow[joint] + jointHigh[joint]) * 0.5f;
                geometry.jointSpheres.push_back({ joint, { middle.x, middle.y, middle.z }, jointRadius[joint] });
            }
        }
    }
    return geometry;
}

uint32_t sceneTime(std::chrono::steady_clock::time_point time)
{
    return uint32_t(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count()));
}

void buildTerrainLod(const TerrainGrid& grid, uint32_t step, std::vector<SceneVertex>& vertices, std::vector<uint16_t>& indices)
{
    vertices.clear();
    indices.clear();
    const uint32_t points = grid.size + 1;
    std::vector<int32_t> made(size_t(points) * points, -1);
    const auto vertex = [&](uint32_t x, uint32_t y) -> uint16_t
    {
        const size_t at = size_t(y) * points + x;
        if (made[at] < 0)
        {
            made[at] = int32_t(vertices.size());
            SceneVertex v{};
            v.position[0] = grid.origin[0] + float(x) * grid.spacing;
            v.position[1] = grid.origin[1] + float(y) * grid.spacing;
            v.position[2] = grid.heights[at];
            memcpy(v.normal, grid.normals + at * 3, sizeof(v.normal));
            v.tangent[0] = 1.f; // x runs east, as the ground's UVs
            v.tangent[3] = 1.f;
            v.uv[0] = grid.blends[at * 2];
            v.uv[1] = grid.blends[at * 2 + 1];
            vertices.push_back(v);
        }
        return uint16_t(made[at]);
    };

    // Counter-clockwise seen from above, as the front of an upward face.
    const uint32_t cells = grid.size / step;
    std::vector<uint16_t> ring;
    for (uint32_t cy = 0; cy < cells; ++cy)
    {
        for (uint32_t cx = 0; cx < cells; ++cx)
        {
            const uint32_t x0 = cx * step;
            const uint32_t y0 = cy * step;
            const uint32_t x1 = x0 + step;
            const uint32_t y1 = y0 + step;
            const bool south = y0 == 0;
            const bool east = x1 == grid.size;
            const bool north = y1 == grid.size;
            const bool west = x0 == 0;
            if (step == 1 || !(south || east || north || west))
            {
                const uint16_t a = vertex(x0, y0);
                const uint16_t b = vertex(x1, y0);
                const uint16_t c = vertex(x0, y1);
                const uint16_t d = vertex(x1, y1);
                indices.insert(indices.end(), { a, b, c, b, d, c });
                continue;
            }

            // Round the cell, with every point of the edges that lie on the patch's border.
            ring.clear();
            for (uint32_t x = x0; x < x1; x += south ? 1 : step)
            {
                ring.push_back(vertex(x, y0));
            }
            for (uint32_t y = y0; y < y1; y += east ? 1 : step)
            {
                ring.push_back(vertex(x1, y));
            }
            for (uint32_t x = x1; x > x0; x -= north ? 1 : step)
            {
                ring.push_back(vertex(x, y1));
            }
            for (uint32_t y = y1; y > y0; y -= west ? 1 : step)
            {
                ring.push_back(vertex(x0, y));
            }
            const uint16_t centre = vertex(x0 + step / 2, y0 + step / 2);
            for (size_t i = 0; i < ring.size(); ++i)
            {
                indices.insert(indices.end(), { centre, ring[i], ring[(i + 1) % ring.size()] });
            }
        }
    }
}

SceneCamera3D makeCamera(const float eye[3], const float target[3], float verticalFovRadians, float aspect, float nearClip)
{
    const Vec3 forward = normalized(Vec3{ target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] });
    const Vec3 side = normalized(cross(forward, Vec3{ 0.f, 0.f, 1.f }));
    const Vec3 up = cross(side, forward);
    const float forwardAxis[3] = { forward.x, forward.y, forward.z };
    const float upAxis[3] = { up.x, up.y, up.z };
    return makeCameraFromAxes(eye, forwardAxis, upAxis, verticalFovRadians, aspect, nearClip, 0.f);
}

SceneCamera3D makeCameraFromAxes(const float eye[3], const float forwardAxis[3], const float upAxis[3], float verticalFovRadians, float aspect,
                                 float nearClip, float farClip)
{
    const Vec3 position{ eye[0], eye[1], eye[2] };
    const Vec3 forward{ forwardAxis[0], forwardAxis[1], forwardAxis[2] };
    const Vec3 up{ upAxis[0], upAxis[1], upAxis[2] };
    const Vec3 side = cross(forward, up);

    const float focal = 1.f / std::tan(verticalFovRadians * 0.5f);
    // Columns of the view matrix: world axes and translation in camera space, looking down -Z.
    const float viewColumns[4][4] = { { side.x, up.x, -forward.x, 0.f },
                                      { side.y, up.y, -forward.y, 0.f },
                                      { side.z, up.z, -forward.z, 0.f },
                                      { -dot(side, position), -dot(up, position), dot(forward, position), 1.f } };

    SceneCamera3D camera{};
    float* m = camera.viewProjection;
    for (int c = 0; c < 4; ++c)
    {
        const float* view = viewColumns[c];
        m[c * 4 + 0] = view[0] * focal / aspect;
        m[c * 4 + 1] = -view[1] * focal;
        m[c * 4 + 2] = view[3] * nearClip;
        m[c * 4 + 3] = -view[2];
        for (int row = 0; row < 3; ++row)
        {
            camera.viewRows[row][c] = view[row];
        }
    }
    camera.position[0] = eye[0];
    camera.position[1] = eye[1];
    camera.position[2] = eye[2];
    camera.focal[0] = focal / aspect;
    camera.focal[1] = focal;
    camera.nearClip = nearClip;
    camera.lodScale = 1.f;
    camera.farClip = farClip;
    return camera;
}

GpuSceneCounts sceneCountsTemplate()
{
    GpuSceneCounts counts{};
    for (uint32_t list = 0; list < SCENE_LISTS; ++list)
    {
        const uint32_t vertices = CLUSTER_BUCKET_TRIANGLES[list % CLUSTER_BUCKETS] * 3;
        counts.early[list].vertexCount = vertices;
        counts.late[list].vertexCount = vertices;
    }
    counts.selection.vertexCount = CLUSTER_BUCKET_TRIANGLES[CLUSTER_BUCKETS - 1] * 3; // one draw for every size

    for (GpuWorkCount* work : { &counts.retest, &counts.groups[0], &counts.groups[1] })
    {
        work->groups[1] = 1;
        work->groups[2] = 1;
    }
    return counts;
}

uint32_t pyramidLayout(uint32_t width, uint32_t height, uint32_t offsets[PYRAMID_MAX_LEVELS], uint32_t& levels)
{
    uint32_t total = 0;
    levels = 0;
    for (uint32_t level = 1; level <= PYRAMID_MAX_LEVELS; ++level)
    {
        const uint32_t levelWidth = pyramidLevelSize(width, level);
        const uint32_t levelHeight = pyramidLevelSize(height, level);
        offsets[level - 1] = total;
        total += levelWidth * levelHeight;
        levels = level;
        if (levelWidth == 1 && levelHeight == 1)
        {
            break;
        }
    }
    return total;
}

void fillSceneView(GpuSceneView& view, const SceneCamera3D& camera, const SceneCamera3D* previous, uint32_t width, uint32_t height)
{
    memcpy(view.viewProjection, camera.viewProjection, sizeof(view.viewProjection));
    view.camera[0] = camera.position[0];
    view.camera[1] = camera.position[1];
    view.camera[2] = camera.position[2];
    view.camera[3] = 1.f;
    memcpy(view.viewRows, camera.viewRows, sizeof(view.viewRows));
    view.projection[0] = camera.focal[0];
    view.projection[1] = camera.focal[1];
    view.projection[2] = camera.nearClip;
    view.projection[3] = camera.lodScale;
    if (previous)
    {
        memcpy(view.previousRows, previous->viewRows, sizeof(view.previousRows));
        view.previousProjection[0] = previous->focal[0];
        view.previousProjection[1] = previous->focal[1];
        view.previousProjection[2] = previous->nearClip;
        view.previousProjection[3] = 1.f;
    }
    pyramidLayout(width, height, view.pyramidOffset, view.pyramidLevels);

    // Each plane comes from the clip rule it stands for, as a sum over the matrix rows.
    const float* m = camera.viewProjection;
    const auto row = [m](int index, float out[4])
    {
        for (int c = 0; c < 4; ++c)
        {
            out[c] = m[c * 4 + index];
        }
    };
    float x[4], y[4], z[4], w[4];
    row(0, x);
    row(1, y);
    row(2, z);
    row(3, w);
    for (int c = 0; c < 4; ++c)
    {
        view.planes[0][c] = w[c] + x[c];
        view.planes[1][c] = w[c] - x[c];
        view.planes[2][c] = w[c] + y[c];
        view.planes[3][c] = w[c] - y[c];
        view.planes[4][c] = w[c] - z[c]; // reversed depth: z <= w is the near plane
    }
    for (int plane = 0; plane < 5; ++plane)
    {
        float* value = view.planes[plane];
        const float size = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
        for (int c = 0; c < 4; ++c)
        {
            value[c] /= size;
        }
    }

    // Reversed depth has no far plane of its own, and everything passes this one. The limit
    // is farClip, a distance: a plane would reach further at the sides of a wide view.
    view.planes[5][3] = 1.f;
    view.farClip = camera.farClip;
    view.pixelSize[0] = float(width);
    view.pixelSize[1] = float(height);
    view.pixelSize[2] = 1.f / float(width);
    view.pixelSize[3] = 1.f / float(height);
}

SceneQueue::SceneQueue() = default;

uint32_t SceneQueue::addGeometry(GeometryData&& data, uint32_t coarser, float coarserAt)
{
    GeometryCommand command;
    command.data = std::move(data);
    command.coarser = coarser;
    command.coarserAt = coarserAt;

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mFreeGeometries.empty())
    {
        command.id = mFreeGeometries.back();
        mFreeGeometries.pop_back();
    }
    else
    {
        command.id = mNextGeometry++;
    }
    const uint32_t id = command.id;
    mCommands.geometries.push_back(std::move(command));
    return id;
}

void SceneQueue::removeGeometry(uint32_t id)
{
    if (id != 0)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.removedGeometries.push_back(id);
    }
}

uint32_t SceneQueue::addMaterial(const MaterialDesc& desc)
{
    std::lock_guard<std::mutex> lock(mMutex);
    uint32_t id = 0;
    if (!mFreeMaterials.empty())
    {
        id = mFreeMaterials.back();
        mFreeMaterials.pop_back();
    }
    else
    {
        id = mNextMaterial++;
    }
    mCommands.materials.push_back({ id, desc });
    return id;
}

void SceneQueue::setMaterial(uint32_t id, const MaterialDesc& desc)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.materials.push_back({ id, desc });
}

void SceneQueue::removeMaterial(uint32_t id)
{
    if (id != 0)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.removedMaterials.push_back(id);
    }
}

uint32_t SceneQueue::addTerrain(const TerrainDesc& desc)
{
    std::lock_guard<std::mutex> lock(mMutex);
    uint32_t id = 0;
    if (!mFreeTerrains.empty())
    {
        id = mFreeTerrains.back();
        mFreeTerrains.pop_back();
    }
    else
    {
        id = mNextTerrain++;
    }
    mCommands.terrains.push_back({ id, desc });
    return id;
}

void SceneQueue::setTerrain(uint32_t id, const TerrainDesc& desc)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.terrains.push_back({ id, desc });
}

void SceneQueue::removeTerrain(uint32_t id)
{
    if (id != 0)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.removedTerrains.push_back(id);
    }
}

uint32_t SceneQueue::addInstance(const InstanceDesc& desc)
{
    InstanceCommand command;
    command.desc = desc;

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mFreeInstances.empty())
    {
        command.id = mFreeInstances.back();
        mFreeInstances.pop_back();
    }
    else
    {
        command.id = mNextInstance++;
    }
    const uint32_t id = command.id;
    mCommands.instances.push_back(command);
    return id;
}

void SceneQueue::setInstance(uint32_t id, const InstanceDesc& desc)
{
    InstanceCommand command;
    command.id = id;
    command.desc = desc;

    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.instances.push_back(command);
}

void SceneQueue::removeInstance(uint32_t id)
{
    InstanceCommand command;
    command.type = InstanceCommand::Type::Remove;
    command.id = id;

    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.instances.push_back(command);
}

uint32_t SceneQueue::addSkeleton(uint32_t jointCount)
{
    SkeletonCommand command;
    command.jointCount = std::max(jointCount, 1u);

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mFreeSkeletons.empty())
    {
        command.id = mFreeSkeletons.back();
        mFreeSkeletons.pop_back();
    }
    else
    {
        command.id = mNextSkeleton++;
    }
    const uint32_t id = command.id;
    mCommands.skeletons.push_back(std::move(command));
    return id;
}

void SceneQueue::poseSkeleton(uint32_t id, const float* rows, uint32_t jointCount, std::chrono::steady_clock::time_point time,
                              std::chrono::microseconds interpolate)
{
    if (id == 0 || jointCount == 0)
    {
        return;
    }
    SkeletonCommand command;
    command.id = id;
    command.rows.assign(rows, rows + size_t(jointCount) * 12);
    command.time = sceneTime(time);
    command.interpolate = uint32_t(std::max<int64_t>(0, interpolate.count()));

    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.skeletons.push_back(std::move(command));
}

void SceneQueue::setSkeletonResident(uint32_t id, bool resident)
{
    if (id != 0)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.residentSkeletons.emplace_back(id, resident);
    }
}

void SceneQueue::removeSkeleton(uint32_t id)
{
    if (id != 0)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.removedSkeletons.push_back(id);
    }
}

uint32_t SceneQueue::addSkin(SkinDesc&& desc)
{
    SkinCommand command;
    command.desc = std::move(desc);

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mFreeSkins.empty())
    {
        command.id = mFreeSkins.back();
        mFreeSkins.pop_back();
    }
    else
    {
        command.id = mNextSkin++;
    }
    const uint32_t id = command.id;
    mCommands.skins.push_back(std::move(command));
    return id;
}

void SceneQueue::removeSkin(uint32_t id)
{
    if (id != 0)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.removedSkins.push_back(id);
    }
}

void SceneQueue::take(SceneCommands& commands)
{
    std::lock_guard<std::mutex> lock(mMutex);
    std::swap(commands, mCommands);
}

uint32_t SceneQueue::reserveGeometry()
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mFreeGeometries.empty())
    {
        const uint32_t id = mFreeGeometries.back();
        mFreeGeometries.pop_back();
        return id;
    }
    return mNextGeometry++;
}

uint32_t SceneQueue::takeId(std::vector<uint32_t>& free, uint32_t& next)
{
    if (!free.empty())
    {
        const uint32_t id = free.back();
        free.pop_back();
        return id;
    }
    return next++;
}

uint32_t SceneQueue::addMorphSet(MorphSetDesc&& desc)
{
    MorphSetCommand command;
    command.desc = std::move(desc);
    std::lock_guard<std::mutex> lock(mMutex);
    command.id = takeId(mFreeMorphSets, mNextMorphSet);
    const uint32_t id = command.id;
    mCommands.morphSets.push_back(std::move(command));
    return id;
}

void SceneQueue::removeMorphSet(uint32_t id)
{
    if (id != 0)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.removedMorphSets.push_back(id);
    }
}

uint32_t SceneQueue::addMorph(uint32_t set)
{
    MorphCommand command;
    command.set = set;
    std::lock_guard<std::mutex> lock(mMutex);
    command.id = takeId(mFreeMorphs, mNextMorph);
    const uint32_t id = command.id;
    mCommands.morphs.push_back(std::move(command));
    return id;
}

void SceneQueue::setMorphWeights(uint32_t id, const float* weights, uint32_t count)
{
    MorphCommand command;
    command.id = id;
    command.values.assign(weights, weights + count);
    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.morphs.push_back(std::move(command));
}

void SceneQueue::setMorphMask(uint32_t id, uint32_t target, const float* values, uint32_t count)
{
    MorphCommand command;
    command.id = id;
    command.target = target;
    command.values.assign(values, values + count);
    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.morphs.push_back(std::move(command));
}

void SceneQueue::removeMorph(uint32_t id)
{
    if (id != 0)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.removedMorphs.push_back(id);
    }
}

void SceneQueue::releaseMorphSet(uint32_t id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mFreeMorphSets.push_back(id);
}

void SceneQueue::releaseMorph(uint32_t id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mFreeMorphs.push_back(id);
}

void SceneQueue::releaseSkeleton(uint32_t id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mFreeSkeletons.push_back(id);
}

void SceneQueue::releaseSkin(uint32_t id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mFreeSkins.push_back(id);
}

void SceneQueue::releaseGeometry(uint32_t id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mFreeGeometries.push_back(id);
}

void SceneQueue::releaseInstance(uint32_t id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mFreeInstances.push_back(id);
}

void SceneQueue::releaseMaterial(uint32_t id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mFreeMaterials.push_back(id);
}

void SceneQueue::releaseTerrain(uint32_t id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mFreeTerrains.push_back(id);
}

} // namespace asr
