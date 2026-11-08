/**
 * asrSceneCommon.glsl
 * What every scene shader shares. The structs mirror the Gpu* structs of asrscene.h.
 * Everything is reached through device addresses given in push constants, so tables
 * and geometry chunks need no binding, and one source runs on Vulkan and Metal.
 */
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

const uint LISTS = 16;
const uint PYRAMID_LEVELS = 16;

const uint INSTANCE_FLIPPED = 1u;
const uint INSTANCE_DOUBLE_SIDED = 2u;
const uint INSTANCE_MASKED = 4u;
const uint INSTANCE_RANK_SHIFT = 8u;
const uint INSTANCE_RANK_MAX = 1023u;

struct Instance
{
    vec4 rows[3];
    vec4 lod; // xyz is what LOD measures from, and w the object's size
    uint geometry;
    uint material;
    uint flags;
    uint unused;
};

struct Geometry
{
    uint firstGroup;
    uint groupCount; // 0 when its clusters are tested directly
    uint unused;
    float radius;
    uint firstCluster;
    uint clusterCount;
    uint coarser;
    float coarserAt;
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

// Stored in the cluster table, just before the clusters of its geometry.
struct ClusterGroup
{
    vec3 center;
    float radius;
    uint cone;
    uint firstCluster;
    uint clusterCount;
    uint unused[5];
};

struct Material
{
    vec4 color;
    vec3 uvRow0;
    uint texture;
    vec3 uvRow1;
    float alphaCutoff;
    vec3 planarScale;
    uint planar;
    vec3 planarOffset;
    uint unused;
};

struct ListEntry
{
    uint instance;
    uint cluster;
};

layout(buffer_reference, std430) readonly buffer ViewRef
{
    mat4 viewProjection;
    vec4 camera;
    vec4 planes[6];
    vec4 pixelSize;
    vec4 viewRows[3];
    vec4 previousRows[3];
    vec4 projection;
    vec4 previousProjection;
    uint listBase[LISTS];
    uint pyramidOffset[PYRAMID_LEVELS];
    uint instanceEnd;
    uint pyramidLevels;
    float farClip;
    uint feedback; // texture feedback: (window << 4) | the place sampled in each 4 by 4 block, 0 when off
    uint visibleEnd; // entries of the visible list and of the two tables, for the resolve's checks
    uint clusterEnd;
    uint materialEnd;
    uint unusedEnd;
};

layout(buffer_reference, std430) readonly buffer InstancesRef { Instance items[]; };
layout(buffer_reference, std430) readonly buffer GeometriesRef { Geometry items[]; };
layout(buffer_reference, std430) readonly buffer ClustersRef { Cluster items[]; };
layout(buffer_reference, std430) readonly buffer ClusterGroupsRef { ClusterGroup items[]; }; // the cluster table too
layout(buffer_reference, std430) readonly buffer MaterialsRef { Material items[]; };
layout(buffer_reference, std430) readonly buffer ListRef { ListEntry items[]; };
layout(buffer_reference, std430) readonly buffer WordsRef { uint words[]; };
layout(buffer_reference, std430) buffer FeedbackRef { uint words[]; };

const uint FEEDBACK_LEVEL_BITS = 8;
const uint FEEDBACK_MAX_LEVEL = 15;

layout(push_constant) uniform ScenePush
{
    uint64_t view;
    uint64_t instances;
    uint64_t geometries;
    uint64_t clusters;
    uint64_t materials;
    uint64_t visible;
    uint64_t output_counts; // the indirect draws
    uint64_t retest;
    uint64_t pyramid;
    uint64_t groups; // the group list
    uint64_t feedback; // a word per texture slot
    uint list;
    uint phase;
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

uint pyramidLevelSize(uint pixels, uint level)
{
    return (pixels + (1u << level) - 1u) >> level;
}
