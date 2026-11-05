/**
 * asrSceneCommon.glsl
 * What every scene shader shares. The structs mirror the Gpu* structs of asrscene.h.
 * Everything is reached through device addresses given in push constants, so tables
 * and geometry chunks need no binding, and one source runs on Vulkan and Metal.
 */
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

const uint BUCKETS = 4;

struct Instance
{
    vec4 rows[3];
    uint geometry;
    uint material;
    uint flags;
    uint unused;
};

struct Geometry
{
    vec3 center;
    float radius;
    uint firstCluster;
    uint clusterCount;
    uint unused0;
    uint unused1;
};

struct Cluster
{
    vec3 center;
    float radius;
    uint cone;
    uint triangles;
    uint64_t indices;
    uint64_t positions;
    uint64_t attributes;
};

struct Material
{
    vec4 color;
    vec3 uvRow0;
    uint texture;
    vec3 uvRow1;
    uint flags;
};

struct VisibleCluster
{
    uint instance;
    uint cluster;
};

layout(buffer_reference, std430) readonly buffer ViewRef
{
    mat4 viewProjection;
    vec4 camera;
    vec4 planes[5];
    vec4 pixelSize;
    uint bucketBase[BUCKETS];
    uint instanceEnd;
};

layout(buffer_reference, std430) readonly buffer InstancesRef { Instance items[]; };
layout(buffer_reference, std430) readonly buffer GeometriesRef { Geometry items[]; };
layout(buffer_reference, std430) readonly buffer ClustersRef { Cluster items[]; };
layout(buffer_reference, std430) readonly buffer MaterialsRef { Material items[]; };
layout(buffer_reference, std430) readonly buffer VisibleRef { VisibleCluster items[]; };
layout(buffer_reference, std430) readonly buffer WordsRef { uint words[]; };

layout(push_constant) uniform ScenePush
{
    uint64_t view;
    uint64_t instances;
    uint64_t geometries;
    uint64_t clusters;
    uint64_t materials;
    uint64_t visible;
    uint64_t output_counts; // the indirect draws
    uint bucket;
} push;

// One vertex index of a cluster: indices are 16 bits, read through 32-bit words.
uint clusterIndex(uint64_t indices, uint index)
{
    const uint word = WordsRef(indices).words[index >> 1];
    return (word >> ((index & 1u) * 16u)) & 0xFFFFu;
}

vec3 packedPosition(uint64_t positions, uint vertex)
{
    WordsRef words = WordsRef(positions);
    const uint xy = words.words[vertex * 2u];
    const uint z = words.words[vertex * 2u + 1u];
    return vec3(float(int(xy << 16) >> 16), float(int(xy) >> 16), float(int(z << 16) >> 16)) / 32767.0;
}

vec3 transformPoint(vec4 rows[3], vec3 p)
{
    return vec3(dot(rows[0], vec4(p, 1.0)), dot(rows[1], vec4(p, 1.0)), dot(rows[2], vec4(p, 1.0)));
}
