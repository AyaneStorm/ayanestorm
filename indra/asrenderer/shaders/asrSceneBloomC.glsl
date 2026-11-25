/**
 * asrSceneBloomC.glsl
 * Bloom, as Jimenez's Next Generation Post Processing in Call of Duty: Advanced Warfare
 * (SIGGRAPH 2014): the scene colour halved BLOOM_LEVELS times with a 13-tap filter, then
 * each level added back up into the one above through a 3x3 tent. The composite takes
 * bloomStrength of all light from the scene and puts the chain's mean back, so bloom moves
 * light and adds none. SL glow joins at the first level: glowStrength times the glow (the
 * scene colour's alpha) of each pixel's colour, which the composite adds on top, as
 * OpenGL's glow does.
 *
 * push.phase: the level written. Below BLOOM_UP, it is made from the level above it (level
 * 0 from the scene colour, with Karis's average against fireflies); with BLOOM_UP, the level
 * below it is added into it.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrSceneCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 1, binding = 9, rgba16f) uniform readonly image2D sceneColor;
layout(set = 1, binding = 19, rgba16f) uniform image2D bloom;

#include "asrBloom.glsl"

ivec2 sSceneSize;
uint sLevel;
float sExposure;

// The light bloom spreads from a scene pixel, or a texel of the level above.
vec3 source(ivec2 texel)
{
    if (sLevel == 0u)
    {
        const vec4 color = imageLoad(sceneColor, clamp(texel, ivec2(0), sSceneSize - 1));
        ViewRef view = ViewRef(push.view);
        return max(color.rgb, vec3(0.0)) * (view.bloomStrength + view.glowStrength * clamp(color.a, 0.0, 1.0));
    }
    return bloomTexel(bloomLevelOrigin(sSceneSize, sLevel - 1u), bloomLevelSize(sSceneSize, sLevel - 1u), texel);
}

// What the group reads, loaded once: going down, the 20x20 source texels under its 8x8
// texels and their taps; going up, the 8x8 texels of the level below under them.
const int DOWN_TILE = 20;
const int UP_TILE = 8;
shared vec3 sTile[DOWN_TILE * DOWN_TILE];
ivec2 sTileOrigin; // the tile's first texel, in the source's or the level below's texels

vec3 tileTexel(ivec2 texel, int width)
{
    const ivec2 t = texel - sTileOrigin;
    return sTile[t.y * width + t.x];
}

// The 2x2 texels around a texel corner: what a bilinear tap there reads.
vec3 tap(ivec2 corner)
{
    return 0.25 * (tileTexel(corner - ivec2(1, 1), DOWN_TILE) + tileTexel(corner - ivec2(0, 1), DOWN_TILE)
                   + tileTexel(corner - ivec2(1, 0), DOWN_TILE) + tileTexel(corner, DOWN_TILE));
}

// Bilinear in the level below, at p in its texels.
vec3 tileBilinear(vec2 p)
{
    const vec2 f = p - 0.5;
    const ivec2 i = ivec2(floor(f));
    const vec2 w = f - vec2(i);
    const vec3 top = mix(tileTexel(i, UP_TILE), tileTexel(i + ivec2(1, 0), UP_TILE), w.x);
    const vec3 bottom = mix(tileTexel(i + ivec2(0, 1), UP_TILE), tileTexel(i + ivec2(1, 1), UP_TILE), w.x);
    return mix(top, bottom, w.y);
}

float karis(vec3 color)
{
    return 1.0 / (1.0 + dot(color, vec3(0.2126, 0.7152, 0.0722)) * sExposure);
}

vec3 downsample(ivec2 texel)
{
    // The source texel corner under this texel's centre, and the 13 taps around it.
    const ivec2 c = texel * 2 + 1;
    const vec3 a = tap(c + ivec2(-2, -2)), b = tap(c + ivec2(0, -2)), d = tap(c + ivec2(2, -2));
    const vec3 f = tap(c + ivec2(-2, 0)),  g = tap(c),                h = tap(c + ivec2(2, 0));
    const vec3 k = tap(c + ivec2(-2, 2)),  l = tap(c + ivec2(0, 2)),  m = tap(c + ivec2(2, 2));
    const vec3 i0 = tap(c + ivec2(-1, -1)), i1 = tap(c + ivec2(1, -1)), i2 = tap(c + ivec2(-1, 1)), i3 = tap(c + ivec2(1, 1));
    // Five overlapping boxes: the inner one weighs half, the four corner ones an eighth each.
    const vec3 boxes[5] = vec3[5](0.25 * (i0 + i1 + i2 + i3), 0.25 * (a + b + f + g), 0.25 * (b + d + g + h),
                                  0.25 * (f + g + k + l), 0.25 * (g + h + l + m));
    const float weights[5] = float[5](0.5, 0.125, 0.125, 0.125, 0.125);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int box = 0; box < 5; ++box)
    {
        const float w = sLevel == 0u ? weights[box] * karis(boxes[box]) : weights[box];
        sum += boxes[box] * w;
        total += w;
    }
    return sum / total;
}

void main()
{
    ViewRef view = ViewRef(push.view);
    sSceneSize = ivec2(view.pixelSize);
    sLevel = push.phase & (BLOOM_UP - 1u);
    sExposure = ExposureRef(view.exposure).exposure * view.userExposure;
    const ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 group = ivec2(gl_WorkGroupID.xy) * 8;
    const int local = int(gl_LocalInvocationIndex);
    const bool up = (push.phase & BLOOM_UP) != 0u;
    // Every thread helps load the tile, even outside the level: the barrier waits for all.
    if (up)
    {
        // The level below, from 2 texels before this group's first half-texel to 5 after.
        sTileOrigin = group / 2 - 2;
        const ivec2 belowOrigin = bloomLevelOrigin(sSceneSize, sLevel + 1u);
        const ivec2 belowSize = bloomLevelSize(sSceneSize, sLevel + 1u);
        sTile[local] = bloomTexel(belowOrigin, belowSize, sTileOrigin + ivec2(local % UP_TILE, local / UP_TILE));
    }
    else
    {
        // Taps reach 2 source texels before a texel's pair and 2 after it.
        sTileOrigin = group * 2 - 2;
        for (int i = local; i < DOWN_TILE * DOWN_TILE; i += 64)
        {
            sTile[i] = source(sTileOrigin + ivec2(i % DOWN_TILE, i / DOWN_TILE));
        }
    }
    barrier();

    const ivec2 size = bloomLevelSize(sSceneSize, sLevel);
    if (texel.x >= size.x || texel.y >= size.y)
    {
        return;
    }
    const ivec2 origin = bloomLevelOrigin(sSceneSize, sLevel);
    vec3 result;
    if (up)
    {
        // A 3x3 tent over the level below, one of its texels apart, added to this level.
        const vec2 p = (vec2(texel) + 0.5) * 0.5;
        vec3 tent = vec3(0.0);
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                tent += tileBilinear(p + vec2(x, y)) * float((2 - abs(x)) * (2 - abs(y)));
            }
        }
        result = imageLoad(bloom, origin + texel).rgb + tent * (1.0 / 16.0);
    }
    else
    {
        result = downsample(texel);
    }
    imageStore(bloom, origin + texel, vec4(result, 0.0));
}
