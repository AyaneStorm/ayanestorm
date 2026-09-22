/**
 * AyaneStorm XeGTAO compute depth preparation. Author: chanayane@firestorm.
 * Derived from the Intel XeGTAO implementation (see LICENSE in that repository).
 */
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D source_depth;
uniform vec2 gtao_projection_depth;
uniform int gtao_orthographic;
uniform float gtao_radius;
uniform ivec2 gtao_viewport;

float gtao_depth_mip_filter(vec4 depth, float radius);

layout(binding = 0, r16f) uniform writeonly image2D gtao_depth_0;
layout(binding = 1, r16f) uniform writeonly image2D gtao_depth_1;
layout(binding = 2, r16f) uniform writeonly image2D gtao_depth_2;
layout(binding = 3, r16f) uniform writeonly image2D gtao_depth_3;
layout(binding = 4, r16f) uniform writeonly image2D gtao_depth_4;

shared float gtao_shared_depth[8][8];

float gtao_linear_depth(ivec2 pixel)
{
    ivec2 clamped_pixel = clamp(pixel, ivec2(0), gtao_viewport - 1);
    float raw_depth = texelFetch(source_depth, clamped_pixel, 0).r;
    float ndc_z = raw_depth * 2.0 - 1.0;
    float result;
    if (gtao_orthographic != 0)
    {
        result = -(ndc_z - gtao_projection_depth.y) / gtao_projection_depth.x;
    }
    else
    {
        result = gtao_projection_depth.y / (ndc_z + gtao_projection_depth.x);
    }
    return (isnan(result) || isinf(result) || result <= 0.0 || raw_depth >= 1.0)
        ? 65504.0 : min(result, 65504.0);
}

void main()
{
    ivec2 base = ivec2(gl_GlobalInvocationID.xy) * 2;
    vec4 depth = vec4(gtao_linear_depth(base),
                      gtao_linear_depth(base + ivec2(1, 0)),
                      gtao_linear_depth(base + ivec2(0, 1)),
                      gtao_linear_depth(base + ivec2(1, 1)));
    if (base.x < gtao_viewport.x && base.y < gtao_viewport.y) imageStore(gtao_depth_0, base, depth.xxxx);
    if (base.x + 1 < gtao_viewport.x && base.y < gtao_viewport.y) imageStore(gtao_depth_0, base + ivec2(1, 0), depth.yyyy);
    if (base.x < gtao_viewport.x && base.y + 1 < gtao_viewport.y) imageStore(gtao_depth_0, base + ivec2(0, 1), depth.zzzz);
    if (base.x + 1 < gtao_viewport.x && base.y + 1 < gtao_viewport.y) imageStore(gtao_depth_0, base + ivec2(1, 1), depth.wwww);

    ivec2 mip1_size = max(ivec2(1), gtao_viewport / 2);
    ivec2 mip1_pixel = ivec2(gl_GlobalInvocationID.xy);
    float mip1 = gtao_depth_mip_filter(depth, gtao_radius);
    if (all(lessThan(mip1_pixel, mip1_size))) imageStore(gtao_depth_1, mip1_pixel, vec4(mip1));
    gtao_shared_depth[gl_LocalInvocationID.x][gl_LocalInvocationID.y] = mip1;
    barrier();

    uvec2 local = gl_LocalInvocationID.xy;
    if (all(equal(local & uvec2(1), uvec2(0))))
    {
        vec4 values = vec4(gtao_shared_depth[local.x][local.y],
                           gtao_shared_depth[local.x + 1u][local.y],
                           gtao_shared_depth[local.x][local.y + 1u],
                           gtao_shared_depth[local.x + 1u][local.y + 1u]);
        float value = gtao_depth_mip_filter(values, gtao_radius);
        ivec2 pixel = mip1_pixel / 2;
        if (all(lessThan(pixel, max(ivec2(1), gtao_viewport / 4)))) imageStore(gtao_depth_2, pixel, vec4(value));
        gtao_shared_depth[local.x][local.y] = value;
    }
    barrier();

    if (all(equal(local & uvec2(3), uvec2(0))))
    {
        vec4 values = vec4(gtao_shared_depth[local.x][local.y],
                           gtao_shared_depth[local.x + 2u][local.y],
                           gtao_shared_depth[local.x][local.y + 2u],
                           gtao_shared_depth[local.x + 2u][local.y + 2u]);
        float value = gtao_depth_mip_filter(values, gtao_radius);
        ivec2 pixel = mip1_pixel / 4;
        if (all(lessThan(pixel, max(ivec2(1), gtao_viewport / 8)))) imageStore(gtao_depth_3, pixel, vec4(value));
        gtao_shared_depth[local.x][local.y] = value;
    }
    barrier();

    if (all(equal(local & uvec2(7), uvec2(0))))
    {
        vec4 values = vec4(gtao_shared_depth[0][0], gtao_shared_depth[4][0],
                           gtao_shared_depth[0][4], gtao_shared_depth[4][4]);
        float value = gtao_depth_mip_filter(values, gtao_radius);
        ivec2 pixel = mip1_pixel / 8;
        if (all(lessThan(pixel, max(ivec2(1), gtao_viewport / 16)))) imageStore(gtao_depth_4, pixel, vec4(value));
    }
}
