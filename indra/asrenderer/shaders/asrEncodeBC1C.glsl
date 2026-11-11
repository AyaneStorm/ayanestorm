/**
 * asrEncodeBC1C.glsl
 * BC1 for opaque colour, a block a thread: the endpoints on the colours' principal axis,
 * then once fitted again by least squares to the indices they gave, keeping the better.
 * Always the four-colour mode: these textures have no alpha.
 */
#version 460
#extension GL_GOOGLE_include_directive : require
#include "asrEncodeCommon.glsl"

layout(local_size_x = ENCODE_GROUP_SIZE) in;

uint to565(vec3 c)
{
    const uvec3 q = uvec3(clamp(round(c * vec3(31.0, 63.0, 31.0) / 255.0), vec3(0.0), vec3(31.0, 63.0, 31.0)));
    return (q.r << 11) | (q.g << 5) | q.b;
}

vec3 from565(uint v)
{
    const uvec3 q = uvec3((v >> 11) & 31u, (v >> 5) & 63u, v & 31u);
    return vec3((q.r << 3) | (q.r >> 2), (q.g << 2) | (q.g >> 4), (q.b << 3) | (q.b >> 2));
}

// Indices for c0 above c1, and their squared error. Index 0 is c0, 1 is c1, 2 and 3 the
// thirds between them.
float chooseIndices(uint c0, uint c1, out uint bits)
{
    const vec3 p0 = from565(c0);
    const vec3 p1 = from565(c1);
    const vec3 palette[4] = vec3[4](p0, p1, (2.0 * p0 + p1) / 3.0, (p0 + 2.0 * p1) / 3.0);
    bits = 0u;
    float error = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        uint best = 0u;
        float bestError = 1e30;
        for (uint k = 0u; k < 4u; ++k)
        {
            const vec3 d = palette[k] - texel(i).rgb;
            const float e = dot(d, d);
            if (e < bestError)
            {
                bestError = e;
                best = k;
            }
        }
        bits |= best << (2u * i);
        error += bestError;
    }
    return error;
}

// The endpoints that best fit these indices, by least squares.
bool refit(uint bits, out vec3 a, out vec3 b)
{
    const float WEIGHT[4] = float[4](0.0, 1.0, 1.0 / 3.0, 2.0 / 3.0); // how far toward c1
    float aa = 0.0, ab = 0.0, bb = 0.0;
    vec3 ax = vec3(0.0), bx = vec3(0.0);
    for (uint i = 0u; i < 16u; ++i)
    {
        const float t = WEIGHT[(bits >> (2u * i)) & 3u];
        aa += (1.0 - t) * (1.0 - t);
        ab += (1.0 - t) * t;
        bb += t * t;
        ax += (1.0 - t) * texel(i).rgb;
        bx += t * texel(i).rgb;
    }
    const float det = aa * bb - ab * ab;
    if (abs(det) < 1e-6)
    {
        return false;
    }
    a = clamp((ax * bb - bx * ab) / det, 0.0, 255.0);
    b = clamp((bx * aa - ax * ab) / det, 0.0, 255.0);
    return true;
}

// Endpoints in the order the four-colour mode needs; equal ones give one colour.
float encode(vec3 a, vec3 b, out uvec2 block)
{
    uint c0 = to565(a);
    uint c1 = to565(b);
    if (c0 < c1)
    {
        const uint t = c0;
        c0 = c1;
        c1 = t;
    }
    uint bits = 0u;
    float error = 0.0;
    if (c0 == c1)
    {
        const vec3 p = from565(c0);
        for (uint i = 0u; i < 16u; ++i)
        {
            const vec3 d = p - texel(i).rgb;
            error += dot(d, d);
        }
    }
    else
    {
        error = chooseIndices(c0, c1, bits);
    }
    block = uvec2(c0 | (c1 << 16), bits);
    return error;
}

void main()
{
    const uint block = loadBlock();
    if (block == ~0u)
    {
        return;
    }
    vec4 lo, hi;
    fitLine(0xFFFFu, vec4(1.0, 1.0, 1.0, 0.0), lo, hi);
    uvec2 best;
    const float bestError = encode(hi.rgb, lo.rgb, best);
    vec3 a, b;
    if (bestError > 0.0 && refit(best.y, a, b))
    {
        // encode() orders the endpoints again and chooses new indices for them.
        uvec2 other;
        const float otherError = encode(a, b, other);
        if (otherError < bestError)
        {
            best = other;
        }
    }
    EncodeWordsRef words = EncodeWordsRef(push.instances);
    words.words[block * 2u] = best.x;
    words.words[block * 2u + 1u] = best.y;
}
