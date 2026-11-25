/**
 * asrBloom.glsl
 * Where bloom's levels sit in their atlas, and filtered reads inside one level. The atlas is
 * read through the storage binding (19), so the passes that write it can read it too: the
 * includer declares `bloom` before including this. Layout as asrscene.h's bloomAtlas*().
 */

const uint BLOOM_LEVELS = 6u;   // asrscene.h
const uint BLOOM_UP = 0x100u;

ivec2 bloomLevelSize(ivec2 scene, uint level)
{
    const int divisor = 2 << level;
    return max((scene + divisor - 1) / divisor, ivec2(1));
}

// Level 0 on the left, the others down a column to its right.
ivec2 bloomLevelOrigin(ivec2 scene, uint level)
{
    if (level == 0u)
    {
        return ivec2(0);
    }
    ivec2 origin = ivec2(bloomLevelSize(scene, 0u).x, 0);
    for (uint below = 1u; below < level; ++below)
    {
        origin.y += bloomLevelSize(scene, below).y;
    }
    return origin;
}

// A texel of a level, clamped to its edge so no level bleeds into the next.
vec3 bloomTexel(ivec2 origin, ivec2 size, ivec2 texel)
{
    return imageLoad(bloom, origin + clamp(texel, ivec2(0), size - 1)).rgb;
}

// Bilinear, at p in the level's texels (texel centres at i + 0.5).
vec3 bloomBilinear(ivec2 origin, ivec2 size, vec2 p)
{
    const vec2 f = p - 0.5;
    const ivec2 i = ivec2(floor(f));
    const vec2 w = f - vec2(i);
    const vec3 top = mix(bloomTexel(origin, size, i), bloomTexel(origin, size, i + ivec2(1, 0)), w.x);
    const vec3 bottom = mix(bloomTexel(origin, size, i + ivec2(0, 1)), bloomTexel(origin, size, i + ivec2(1, 1)), w.x);
    return mix(top, bottom, w.y);
}
