/**
 * asrSceneResolveC.glsl
 * Turns the visibility buffer into G-buffer pixels: each visible pixel is shaded once,
 * whatever was drawn over or under it. Barycentrics and their screen derivatives come
 * from the triangle's three vertices, which gives texture sampling the same gradients a
 * fragment shader would have had.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform texture2D textures[65536];
layout(set = 0, binding = 1) uniform sampler samplers[4];
layout(set = 1, binding = 0, r32ui) uniform readonly uimage2D visibility;
layout(set = 1, binding = 1, rgba8) uniform writeonly image2D albedo;
layout(set = 1, binding = 2, rgba16f) uniform writeonly image2D normals;

struct Interpolator
{
    vec3 weights;
    vec3 ddx;
    vec3 ddy;
};

// Perspective-correct weights of the pixel inside the triangle, and how they change one
// pixel to the right and one pixel down.
Interpolator interpolator(vec4 p0, vec4 p1, vec4 p2, vec2 ndc, vec2 pixelStep)
{
    const vec3 invW = 1.0 / vec3(p0.w, p1.w, p2.w);
    const vec2 n0 = p0.xy * invW.x;
    const vec2 n1 = p1.xy * invW.y;
    const vec2 n2 = p2.xy * invW.z;

    const float invDet = 1.0 / determinant(mat2(n2 - n1, n0 - n1));
    const vec3 dx = vec3(n1.y - n2.y, n2.y - n0.y, n0.y - n1.y) * invDet * invW;
    const vec3 dy = vec3(n2.x - n1.x, n0.x - n2.x, n1.x - n0.x) * invDet * invW;
    const float dxSum = dx.x + dx.y + dx.z;
    const float dySum = dy.x + dy.y + dy.z;

    const vec2 delta = ndc - n0;
    const float pixelInvW = invW.x + delta.x * dxSum + delta.y * dySum;
    const vec3 raw = vec3(invW.x, 0.0, 0.0) + delta.x * dx + delta.y * dy;

    Interpolator result;
    result.weights = raw / pixelInvW;
    result.ddx = (raw + dx * pixelStep.x) / (pixelInvW + dxSum * pixelStep.x) - result.weights;
    result.ddy = (raw + dy * pixelStep.y) / (pixelInvW + dySum * pixelStep.y) - result.weights;
    return result;
}

// Temporary, while the Mac's GPU page faults here are found: every index and address read
// through is checked first, and a pixel that fails is drawn magenta and read no further.
// The bits are GUARD_* of asrscene.h. They sit at the end of the counts, which are read back.
const uint GUARD_ENTRY = 1u;
const uint GUARD_INSTANCE = 2u;
const uint GUARD_CLUSTER = 4u;
const uint GUARD_MATERIAL = 8u;
const uint GUARD_ADDRESSES = 16u;
const uint64_t CHUNK_BYTES = 64ul * 1024ul * 1024ul; // SCENE_CHUNK_BYTES
const uint64_t GUARD_OFFSET = 560ul;                  // offsetof(GpuSceneCounts, guardBits)

layout(buffer_reference, std430) buffer GuardRef
{
    uint bits;
    uint pixels;
    uint value;
    uint unused;
};

void reject(ivec2 pixel, uint bit, uint value)
{
    GuardRef guard = GuardRef(push.output_counts + GUARD_OFFSET);
    atomicOr(guard.bits, bit);
    atomicAdd(guard.pixels, 1u);
    atomicMax(guard.value, value);
    imageStore(albedo, pixel, vec4(1.0, 0.0, 1.0, 1.0));
    imageStore(normals, pixel, vec4(0.0, 0.0, 1.0, 1.0));
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

vec3 skinNormal(ViewRef view, Binding binding, uint vertex, vec3 normal)
{
    uvec4 joints;
    vec4 amounts;
    skinWeights(binding.weights, vertex, joints, amounts);
    RowsRef palette = RowsRef(view.skinTables[SKIN_TABLE_PALETTE]);
    vec3 turned = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        if (amounts[i] > 0.0)
        {
            turned += amounts[i] * rowsDirection(palette.items[binding.palette + joints[i]], normal);
        }
    }
    return dot(turned, turned) > 0.0 ? normalize(turned) : normal;
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

// Texture feedback, from one pixel of each 4 by 4 block, at a place that moves every frame.
// The samplers are not anisotropic, so the GPU picks its mip from the longer of the two
// steps: the texels across that step makes one per pixel are what this pixel really needs.
// Most pixels find a word already as large, and skip the atomic.
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

// A texture by its bits (see uiTextureBits()), with the steps of its UVs one pixel across and down.
vec4 sampleBits(ivec2 pixel, uint bits, vec2 uv, vec2 uvDx, vec2 uvDy)
{
    const uint slot = bits & 0xFFFFFu;
    const uint sampler_index = (bits >> 28) & 3u;
    giveFeedback(pixel, slot, uvDx, uvDy);
    return textureGrad(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(sampler_index)]), uv, uvDx, uvDy);
}

// A terrain layer through its matrix, from a pair of region coordinates and their steps.
vec4 sampleLayer(ivec2 pixel, TerrainLayer layer, vec2 at, vec2 atDx, vec2 atDy)
{
    const vec2 uv = vec2(dot(layer.uvRow0, vec3(at, 1.0)), dot(layer.uvRow1, vec3(at, 1.0)));
    const vec2 uvDx = vec2(dot(layer.uvRow0.xy, atDx), dot(layer.uvRow1.xy, atDx));
    const vec2 uvDy = vec2(dot(layer.uvRow0.xy, atDy), dot(layer.uvRow1.xy, atDy));
    return sampleBits(pixel, layer.texture, uv, uvDx, uvDy) * layer.color;
}

// As pbrterrainUtilF.glsl's triplanar sampling: each plane's coordinates, flipped on the
// side facing away, weighted by the normal. A face looking down samples the corner texel,
// as there.
vec4 sampleLayerPlanes(ivec2 pixel, Terrain terrain, uint index, vec3 region, vec3 regionDx, vec3 regionDy, vec3 normal)
{
    const TerrainLayer layer = terrain.layers[index];
    if (terrain.planes != 3u)
    {
        return sampleLayer(pixel, layer, region.xy, regionDx.xy, regionDy.xy);
    }

    const float TRIPLANAR_MIX_THRESHOLD = 0.01;
    vec3 weight = pow(abs(normal), vec3(terrain.sharpness));
    weight /= weight.x + weight.y + weight.z;
    weight -= vec3(TRIPLANAR_MIX_THRESHOLD);
    weight = max(weight, vec3(0.0));
    weight /= weight.x + weight.y + weight.z;

    vec4 color = vec4(0.0);
    if (weight.x > 0.0)
    {
        const vec2 flip = normal.x > 0.0 ? vec2(1.0) : vec2(-1.0, 1.0);
        color += weight.x * sampleLayer(pixel, layer, region.yz * flip, regionDx.yz * flip, regionDy.yz * flip);
    }
    if (weight.y > 0.0)
    {
        const vec2 flip = normal.y > 0.0 ? vec2(-1.0, 1.0) : vec2(1.0);
        color += weight.y * sampleLayer(pixel, layer, region.xz * flip, regionDx.xz * flip, regionDy.xz * flip);
    }
    if (weight.z > 0.0)
    {
        if (normal.z > 0.0)
        {
            color += weight.z * sampleLayer(pixel, layer, region.xy, regionDx.xy, regionDy.xy);
        }
        else
        {
            const uint slot = layer.texture & 0xFFFFFu;
            const uint sampler_index = (layer.texture >> 28) & 3u;
            color += weight.z * textureLod(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(sampler_index)]), vec2(0.0), 0.0) * layer.color;
        }
    }
    return color;
}

// The ground's colour, as terrainF.glsl and pbrterrainF.glsl blend it. A vertex's UV is its
// composition, from 0 to 3, and a noise value; the alpha ramp turns them into how much each
// layer shows. Only layers that show are sampled, which gives the same result.
vec4 terrainColor(ivec2 pixel, Terrain terrain, vec3 region, vec3 regionDx, vec3 regionDy, vec2 blend, vec2 blendDx, vec2 blendDy, vec3 normal)
{
    const float alpha1 = sampleBits(pixel, terrain.ramp, blend, blendDx, blendDy).a;
    const float alpha2 = sampleBits(pixel, terrain.ramp, blend - vec2(2.0, 0.0), blendDx, blendDy).a;
    const float alphaFinal = sampleBits(pixel, terrain.ramp, blend - vec2(1.0, 0.0), blendDx, blendDy).a;
    vec4 weights = vec4(alphaFinal * alpha1, alphaFinal * (1.0 - alpha1), (1.0 - alphaFinal) * alpha2, (1.0 - alphaFinal) * (1.0 - alpha2));

    if (terrain.kind == TERRAIN_MATERIALS)
    {
        // get_terrain_mix_weights(): what is under the threshold drops out, and the rest
        // is scaled back up to one.
        const float RAMP_MIX_THRESHOLD = 0.1;
        weights = max(weights - vec4(RAMP_MIX_THRESHOLD), vec4(0.0));
        weights /= weights.x + weights.y + weights.z + weights.w;
    }

    vec4 color = vec4(0.0);
    for (uint i = 0u; i < TERRAIN_LAYERS; ++i)
    {
        if (weights[i] > 0.0)
        {
            color += weights[i] * sampleLayerPlanes(pixel, terrain, i, region, regionDx, regionDy, normal);
        }
    }
    return color;
}

void main()
{
    const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ViewRef view = ViewRef(push.view);
    if (pixel.x >= int(view.pixelSize.x) || pixel.y >= int(view.pixelSize.y))
    {
        return;
    }

    const uint id = imageLoad(visibility, pixel).r;
    if (id == 0xFFFFFFFFu)
    {
        imageStore(albedo, pixel, vec4(0.0, 0.0, 0.0, 0.0));
        imageStore(normals, pixel, vec4(0.0));
        return;
    }

    const uint triangle = id & 63u;
    const uint entry = id >> 6;
    if (entry >= view.visibleEnd)
    {
        reject(pixel, GUARD_ENTRY, entry);
        return;
    }
    const ListEntry visible = ListRef(push.visible).items[entry];
    if (visible.instance >= view.instanceEnd)
    {
        reject(pixel, GUARD_INSTANCE, visible.instance);
        return;
    }
    if (visible.cluster >= view.clusterEnd)
    {
        reject(pixel, GUARD_CLUSTER, visible.cluster);
        return;
    }
    const Cluster cluster = ClustersRef(push.clusters).items[visible.cluster];
    const Instance instance = InstancesRef(push.instances).items[visible.instance];
    if (instance.material >= view.materialEnd)
    {
        reject(pixel, GUARD_MATERIAL, instance.material);
        return;
    }
    // One blob holds a cluster's indices, then positions, then attributes, in one chunk.
    // A skinned cluster's positions are its own, elsewhere.
    const bool skinned = (instance.flags & INSTANCE_SKINNED) != 0u;
    if (!skinned && (cluster.positions < cluster.indices || cluster.attributes < cluster.positions || cluster.attributes - cluster.indices >= CHUNK_BYTES))
    {
        reject(pixel, GUARD_ADDRESSES, uint(min((cluster.attributes - cluster.indices) >> 20, 0xFFFFFFFFul)));
        return;
    }
    const Material material = MaterialsRef(push.materials).items[instance.material];

    const uint i0 = clusterIndex(cluster.indices, triangle * 3u);
    const uint i1 = clusterIndex(cluster.indices, triangle * 3u + 1u);
    const uint i2 = clusterIndex(cluster.indices, triangle * 3u + 2u);
    const mat4 viewProjection = view.viewProjection;
    const vec4 p0 = viewProjection * vec4(transformPoint(instance.rows, packedPosition(cluster.positions, i0)), 1.0);
    const vec4 p1 = viewProjection * vec4(transformPoint(instance.rows, packedPosition(cluster.positions, i1)), 1.0);
    const vec4 p2 = viewProjection * vec4(transformPoint(instance.rows, packedPosition(cluster.positions, i2)), 1.0);

    const vec2 pixelStep = 2.0 * view.pixelSize.zw;
    const vec2 ndc = (vec2(pixel) + 0.5) * pixelStep - 1.0;
    const Interpolator bary = interpolator(p0, p1, p2, ndc, pixelStep);

    // An attribute is four words: normal, tangent, u, v.
    WordsRef attributes = WordsRef(cluster.attributes);
    const uvec3 base = uvec3(i0, i1, i2) * 4u;
    vec2 uv0 = vec2(uintBitsToFloat(attributes.words[base.x + 2u]), uintBitsToFloat(attributes.words[base.x + 3u]));
    vec2 uv1 = vec2(uintBitsToFloat(attributes.words[base.y + 2u]), uintBitsToFloat(attributes.words[base.y + 3u]));
    vec2 uv2 = vec2(uintBitsToFloat(attributes.words[base.z + 2u]), uintBitsToFloat(attributes.words[base.z + 3u]));

    const uvec3 normalWords = uvec3(attributes.words[base.x], attributes.words[base.y], attributes.words[base.z]);
    vec3 n0 = octahedralDecode(float(int(normalWords.x << 16) >> 16) / 32767.0, float(int(normalWords.x) >> 16) / 32767.0);
    vec3 n1 = octahedralDecode(float(int(normalWords.y << 16) >> 16) / 32767.0, float(int(normalWords.y) >> 16) / 32767.0);
    vec3 n2 = octahedralDecode(float(int(normalWords.z << 16) >> 16) / 32767.0, float(int(normalWords.z) >> 16) / 32767.0);
    if (skinned)
    {
        // Only skinned positions are stored: normals turn here with the same palette, as the
        // viewer's rigged shaders turn them. The box matrix only scales, so it leaves them be.
        const Binding binding = BindingsRef(view.skinTables[SKIN_TABLE_BINDINGS]).items[instance.binding];
        n0 = skinNormal(view, binding, i0, n0);
        n1 = skinNormal(view, binding, i1, n1);
        n2 = skinNormal(view, binding, i2, n2);
    }

    const vec3 objectNormal = mat3(n0, n1, n2) * bary.weights;

    vec4 color;
    if (material.mapping == MATERIAL_TERRAIN)
    {
        // Ground: its position in region metres places the layers, and its UVs blend them.
        const mat3 region = mat3(packedPosition(cluster.positions, i0) * material.planarScale + material.planarOffset,
                                 packedPosition(cluster.positions, i1) * material.planarScale + material.planarOffset,
                                 packedPosition(cluster.positions, i2) * material.planarScale + material.planarOffset);
        const mat3x2 blends = mat3x2(uv0, uv1, uv2);
        const Terrain terrain = TerrainsRef(push.terrains).items[material.texture];
        color = terrainColor(pixel, terrain, region * bary.weights, region * bary.ddx, region * bary.ddy, blends * bary.weights, blends * bary.ddx,
                             blends * bary.ddy, objectNormal);
    }
    else
    {
        if (material.mapping == MATERIAL_PLANAR)
        {
            uv0 = planarUV(n0, packedPosition(cluster.positions, i0) * material.planarScale + material.planarOffset);
            uv1 = planarUV(n1, packedPosition(cluster.positions, i1) * material.planarScale + material.planarOffset);
            uv2 = planarUV(n2, packedPosition(cluster.positions, i2) * material.planarScale + material.planarOffset);
        }
        const mat3x2 uvs = mat3x2(uv0, uv1, uv2);

        const vec2 faceUV = uvs * bary.weights;
        const vec2 uv = vec2(dot(material.uvRow0, vec3(faceUV, 1.0)), dot(material.uvRow1, vec3(faceUV, 1.0)));
        const vec2 faceDx = uvs * bary.ddx;
        const vec2 faceDy = uvs * bary.ddy;
        const vec2 uvDx = vec2(dot(material.uvRow0.xy, faceDx), dot(material.uvRow1.xy, faceDx));
        const vec2 uvDy = vec2(dot(material.uvRow0.xy, faceDy), dot(material.uvRow1.xy, faceDy));
        color = material.color * sampleBits(pixel, material.texture, uv, uvDx, uvDy);
    }

    // Normals follow the cofactor matrix, which stays right under any stretch.
    const vec3 row0 = instance.rows[0].xyz;
    const vec3 row1 = instance.rows[1].xyz;
    const vec3 row2 = instance.rows[2].xyz;
    const mat3 cofactor = mat3(cross(row1, row2), cross(row2, row0), cross(row0, row1));
    vec3 worldNormal = normalize(cofactor * objectNormal);

    // The back of a two-sided surface faces the other way. In this space the front
    // of a triangle turns clockwise, and a flipped instance swaps the two.
    if ((instance.flags & INSTANCE_DOUBLE_SIDED) != 0u)
    {
        const vec2 ndc0 = p0.xy / p0.w;
        const vec2 ndc1 = p1.xy / p1.w;
        const vec2 ndc2 = p2.xy / p2.w;
        const bool clockwise = determinant(mat2(ndc1 - ndc0, ndc2 - ndc0)) < 0.0;
        if (clockwise == ((instance.flags & INSTANCE_FLIPPED) != 0u))
        {
            worldNormal = -worldNormal;
        }
    }

    imageStore(albedo, pixel, vec4(color.rgb, 1.0));
    imageStore(normals, pixel, vec4(worldNormal, 1.0));
}
