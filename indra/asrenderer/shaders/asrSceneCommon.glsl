/**
 * asrSceneCommon.glsl
 * What every scene shader shares. The structs mirror the Gpu* structs of asrscene.h.
 * Everything is reached through device addresses given in push constants, so tables
 * and geometry chunks need no binding, and one source runs on Vulkan and Metal.
 */
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

const uint LISTS = 24;
const uint BLENDED_KIND = 4u;   // the first blended kind; see sceneKind()
const uint BLENDED_LISTS = 8u;
const uint PYRAMID_LEVELS = 16;

const uint INSTANCE_FLIPPED = 1u;
const uint INSTANCE_DOUBLE_SIDED = 2u;
const uint INSTANCE_MASKED = 4u;
const uint INSTANCE_SKINNED = 8u;
const uint INSTANCE_BOUND = 16u;
const uint INSTANCE_BLENDED = 32u;
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
    uint64_t macoit; // Mac OIT's per-pixel state: see macoitBytes() in asrscene.h
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

vec3 octahedralDecode(float x, float y)
{
    vec3 n = vec3(x, y, 1.0 - abs(x) - abs(y));
    if (n.z < 0.0)
    {
        const vec2 wrapped = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.x = wrapped.x;
        n.y = wrapped.y;
    }
    return normalize(n);
}

// A vertex's normal, from the first word of its attributes.
vec3 attributeNormal(uint word)
{
    return octahedralDecode(float(int(word << 16) >> 16) / 32767.0, float(int(word) >> 16) / 32767.0);
}

// The viewer's planar mapping: the texture lies flat on the surface, at a size that
// follows the object's scale, not stretched over the face. position is scaled.
vec2 planarUV(vec3 normal, vec3 position)
{
    vec3 binormal;
    if (abs(normal.x) >= 0.5)
    {
        binormal = vec3(0.0, normal.x < 0.0 ? -1.0 : 1.0, 0.0);
    }
    else
    {
        binormal = vec3(normal.y > 0.0 ? -1.0 : 1.0, 0.0, 0.0);
    }
    const vec3 tangent = cross(binormal, normal);
    return vec2(1.0 + dot(binormal, position) * 2.0 - 0.5, -(dot(tangent, position) * 2.0 - 0.5));
}

vec3 transformPoint(vec4 rows[3], vec3 p)
{
    return vec3(dot(rows[0], vec4(p, 1.0)), dot(rows[1], vec4(p, 1.0)), dot(rows[2], vec4(p, 1.0)));
}

uint pyramidLevelSize(uint pixels, uint level)
{
    return (pixels + (1u << level) - 1u) >> level;
}

// Texture feedback, from one pixel of each 4 by 4 block, at a place that moves every frame.
// The samplers are not anisotropic, so the GPU picks its mip from the longer of the two
// steps: the texels across that step makes one per pixel are what this pixel really needs.
// Most pixels find a word already as large, and skip the atomic. The resolve and Mac OIT's
// colour pass both report through here.
void giveFeedback(ivec2 pixel, uint slot, vec2 uvDx, vec2 uvDy)
{
    const uint feedback = ViewRef(push.view).feedback;
    if (feedback != 0u && slot != 0u && ((uint(pixel.x) & 3u) | ((uint(pixel.y) & 3u) << 2)) == (feedback & 15u))
    {
        const float step = max(max(dot(uvDx, uvDx), dot(uvDy, uvDy)), 1e-30);
        const uint level = uint(clamp(ceil(-0.5 * log2(step)), 0.0, float(FEEDBACK_MAX_LEVEL)));
        const uint word = ((feedback >> 4) << FEEDBACK_LEVEL_BITS) | (level + 1u);
        FeedbackRef seen = FeedbackRef(push.feedback);
        if (seen.words[slot] < word)
        {
            atomicMax(seen.words[slot], word);
        }
    }
}

// Mac OIT, shared by its passes and its resolve. See asrscene.h for the state's layout.
const uint MACOIT_LAYERS = 4u;
const uint MACOIT_EMPTY_KEY = 0xFFFFFFFFu;
const uint MACOIT_KEY_BIAS = 0x00800000u;
const float MACOIT_DEPTH_SCALE = 4194303.0;
const float MACOIT_MIN_LAYER_ALPHA = 1.0 / 255.0;

layout(buffer_reference, std430) buffer MacOITStateRef { uint words[]; };

uint macoitPixels()
{
    ViewRef view = ViewRef(push.view);
    return uint(view.pixelSize.x) * uint(view.pixelSize.y);
}

uint macoitKeyWord(uint pixel, uint layer)
{
    return pixel * MACOIT_LAYERS + layer;
}

uint macoitFarthestWord(uint pixel)
{
    return macoitPixels() * MACOIT_LAYERS + pixel;
}

uint macoitCountWord(uint pixel)
{
    return macoitPixels() * (MACOIT_LAYERS + 1u) + pixel;
}

uint macoitKeyDepth(uint key)
{
    return (key - MACOIT_KEY_BIAS) >> 8u;
}

float macoitKeyDepth01(uint key)
{
    return float(macoitKeyDepth(key)) / MACOIT_DEPTH_SCALE;
}

float macoitKeyTransmittance(uint key)
{
    return 1.0 - float((key - MACOIT_KEY_BIAS) & 255u) / 255.0;
}

// The same effective-opaque end point as the resolve's total opacity.
float macoitOpticalDepth(float alpha)
{
    return -log(max(1.0 - alpha, 1.0 / 65536.0));
}
