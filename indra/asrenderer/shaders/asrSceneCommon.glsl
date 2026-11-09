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
const uint INSTANCE_SKINNED = 8u;
const uint INSTANCE_BOUND = 16u;
const uint INSTANCE_RANK_SHIFT = 8u;
const uint INSTANCE_RANK_MAX = 1023u;

struct Instance
{
    vec4 rows[3];
    vec4 lod; // xyz is what LOD measures from, and w the object's size
    uint geometry;
    uint material;
    uint flags;
    uint binding; // with INSTANCE_BOUND
};

// Avatars: see asrscene.h and asrSceneSkinC.glsl.
const uint BINDING_ATTACHED = 1u;
const uint BINDING_SKINNED = 2u;

struct Rows
{
    vec4 r[3]; // an affine matrix, three rows
};

struct Joint
{
    Rows newest;
    Rows previous;
};

struct Skeleton
{
    vec3 boxCenter;
    float boxScale;
    uint firstJoint;
    uint jointCount;
    uint time;
    uint previousTime;
    float maxLead;
    uint unused[3];
};

struct PaletteJob
{
    uint joint;
    uint matrix;
    uint skeleton;
    uint unused;
};

struct Binding
{
    uint kind;
    uint instance;
    uint skeleton;
    uint palette;     // skinned: first palette entry; attached: its joint
    uint matrix;
    uint source;
    uint target;
    uint targetEntry;
    vec4 lod;
    vec4 sourceBox;
    uint positions;
    uint chosen;
    uint firstBlock;
    uint groupCapacity;
    uint64_t weights;
    uint morph;
    uint unused;
};

struct Morph
{
    uint firstVertex;
    uint vertexCount;
    uint weights;
    uint masks;
    uint outputIndex;
    float normalSoften;
    uint unused[2];
};

// GpuSceneView::skinTables, in SceneTable order.
const uint SKIN_TABLE_SKELETONS = 0u;
const uint SKIN_TABLE_JOINTS = 1u;
const uint SKIN_TABLE_MATRICES = 2u;
const uint SKIN_TABLE_PALETTE_JOBS = 3u;
const uint SKIN_TABLE_PALETTE = 4u;
const uint SKIN_TABLE_BINDINGS = 5u;
const uint SKIN_TABLE_BLOCKS = 6u;
const uint SKIN_TABLE_POSITIONS = 7u;
const uint SKIN_TABLE_MORPHS = 8u;
const uint SKIN_TABLE_MORPH_VERTICES = 9u;
const uint SKIN_TABLE_MORPH_DELTAS = 10u;
const uint SKIN_TABLE_MORPH_WEIGHTS = 11u;
const uint SKIN_TABLE_MORPHED_POSITIONS = 12u;
const uint SKIN_TABLE_MORPHED_ATTRIBUTES = 13u;
const uint SKIN_TABLE_MORPH_WORK = 14u;

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
    uint mapping; // MATERIAL_*
    vec3 planarOffset;
    uint unused;
};

const uint MATERIAL_FACE_UV = 0u;
const uint MATERIAL_PLANAR = 1u;
const uint MATERIAL_TERRAIN = 2u; // texture is a terrain's id

struct TerrainLayer
{
    vec3 uvRow0;
    uint texture;
    vec3 uvRow1;
    uint unused;
    vec4 color;
};

const uint TERRAIN_LAYERS = 4u;
const uint TERRAIN_TEXTURES = 0u;
const uint TERRAIN_MATERIALS = 1u;

struct Terrain
{
    TerrainLayer layers[TERRAIN_LAYERS];
    uint ramp;
    uint kind;
    uint planes;
    float sharpness;
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
    uint groupEnd;   // entries of the group list; the retest list holds visibleEnd + groupEnd + instanceEnd
    uint64_t skinTables[15];
    uint64_t drawOnly; // not 0: a bit per skeleton, and only instances bound to one set are drawn
    uint time;
    uint paletteEnd;
    uint bindingEnd;
    uint blockEnd;
    uint morphWorkEnd;
    uint viewUnused[3];
};

layout(buffer_reference, std430) readonly buffer InstancesRef { Instance items[]; };
layout(buffer_reference, std430) readonly buffer GeometriesRef { Geometry items[]; };
layout(buffer_reference, std430) readonly buffer ClustersRef { Cluster items[]; };
layout(buffer_reference, std430) readonly buffer ClusterGroupsRef { ClusterGroup items[]; }; // the cluster table too
layout(buffer_reference, std430) readonly buffer MaterialsRef { Material items[]; };
layout(buffer_reference, std430) readonly buffer TerrainsRef { Terrain items[]; };
layout(buffer_reference, std430) readonly buffer ListRef { ListEntry items[]; };
layout(buffer_reference, std430) readonly buffer WordsRef { uint words[]; };
layout(buffer_reference, std430) buffer FeedbackRef { uint words[]; };
layout(buffer_reference, std430) readonly buffer BindingsRef { Binding items[]; };
layout(buffer_reference, std430) readonly buffer RowsRef { Rows items[]; };

// A point or a direction through an affine matrix of three rows.
vec3 rowsPoint(Rows m, vec3 p)
{
    return vec3(dot(m.r[0], vec4(p, 1.0)), dot(m.r[1], vec4(p, 1.0)), dot(m.r[2], vec4(p, 1.0)));
}

vec3 rowsDirection(Rows m, vec3 d)
{
    return vec3(dot(m.r[0].xyz, d), dot(m.r[1].xyz, d), dot(m.r[2].xyz, d));
}

// A skinned vertex's palette entries and weights, from its three words of SkinWeights.
void skinWeights(uint64_t weights, uint vertex, out uvec4 joints, out vec4 amounts)
{
    WordsRef words = WordsRef(weights);
    const uint packedJoints = words.words[vertex * 3u];
    const uint low = words.words[vertex * 3u + 1u];
    const uint high = words.words[vertex * 3u + 2u];
    joints = uvec4(packedJoints & 0xFFu, (packedJoints >> 8) & 0xFFu, (packedJoints >> 16) & 0xFFu, packedJoints >> 24);
    amounts = vec4(float(low & 0xFFFFu), float(low >> 16), float(high & 0xFFFFu), float(high >> 16)) / 65535.0;
}

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
    uint64_t terrains;
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
