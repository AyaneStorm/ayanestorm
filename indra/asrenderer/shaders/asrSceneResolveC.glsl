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
    const ListEntry visible = ListRef(push.visible).items[id >> 6];
    const Cluster cluster = ClustersRef(push.clusters).items[visible.cluster];
    const Instance instance = InstancesRef(push.instances).items[visible.instance];
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
    const vec3 n0 = octahedralDecode(float(int(normalWords.x << 16) >> 16) / 32767.0, float(int(normalWords.x) >> 16) / 32767.0);
    const vec3 n1 = octahedralDecode(float(int(normalWords.y << 16) >> 16) / 32767.0, float(int(normalWords.y) >> 16) / 32767.0);
    const vec3 n2 = octahedralDecode(float(int(normalWords.z << 16) >> 16) / 32767.0, float(int(normalWords.z) >> 16) / 32767.0);

    if (material.planar != 0u)
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

    const uint slot = material.texture & 0xFFFFFu;
    const uint sampler_index = (material.texture >> 28) & 3u;
    const vec4 texel = textureGrad(sampler2D(textures[nonuniformEXT(slot)], samplers[nonuniformEXT(sampler_index)]), uv, uvDx, uvDy);

    // Texture feedback, from one pixel of each 4 by 4 block, at a place that moves every
    // frame. The samplers are not anisotropic, so the GPU picks its mip from the longer of
    // the two steps: the texels across that step makes one per pixel are what this pixel
    // really needs. Most pixels find a word already as large, and skip the atomic.
    const uint feedback = view.feedback;
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

    const vec3 objectNormal = mat3(n0, n1, n2) * bary.weights;

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

    imageStore(albedo, pixel, vec4((material.color * texel).rgb, 1.0));
    imageStore(normals, pixel, vec4(worldNormal, 1.0));
}
