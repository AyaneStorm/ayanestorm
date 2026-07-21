// <AS:Chanayane> Exact per-pixel transparency composite
/**
 * Exact per-pixel linked-list transparency composite.
 * Each invocation owns one pixel's list, sorts it far-to-near, then evaluates
 * the original OpenGL blend factors over the preserved opaque scene color.
 */

/*[EXTRA_CODE_HERE]*/

layout(binding = 0, r32ui) uniform coherent uimage2D oitHeadPointers;
layout(binding = 1, r32ui) uniform coherent uimage2D oitListCounts;

struct OITNode
{
    vec4 color;
    // <AS:Chanayane> Scalar glow and implicit node-index sequence keep this node at 32 bytes.
    float glow;
    float depth;
    uint next;
    uint blend;
    // </AS:Chanayane>
};

layout(std430, binding = 0) buffer OITNodes
{
    OITNode oitNodes[];
};

layout(std430, binding = 1) buffer OITControl
{
    uint oitNodeCount;
    uint oitNodeCapacity;
    uint oitOverflow;
    uint oitPad;
};

uniform sampler2D diffuseRect;
uniform int oitDebugMode;
uniform int oitPass;

in vec2 vary_fragcoord;
out vec4 frag_color;

const uint OIT_NULL = 0xffffffffu;

vec4 blend_factor(uint factor, vec4 src, vec4 dst)
{
    if (factor == 0u) return vec4(1.0);                 // ONE
    if (factor == 1u) return vec4(0.0);                 // ZERO
    if (factor == 2u) return dst;                       // DEST_COLOR
    if (factor == 3u) return src;                       // SOURCE_COLOR
    if (factor == 4u) return vec4(1.0) - dst;           // ONE_MINUS_DEST_COLOR
    if (factor == 5u) return vec4(1.0) - src;           // ONE_MINUS_SOURCE_COLOR
    if (factor == 6u) return vec4(dst.a);                // DEST_ALPHA
    if (factor == 7u) return vec4(src.a);                // SOURCE_ALPHA
    if (factor == 8u) return vec4(1.0 - dst.a);          // ONE_MINUS_DEST_ALPHA
    if (factor == 9u) return vec4(1.0 - src.a);          // ONE_MINUS_SOURCE_ALPHA
    return vec4(0.0);
}

bool comes_first(uint lhs, uint rhs)
{
    float ld = oitNodes[lhs].depth;
    float rd = oitNodes[rhs].depth;
    // <AS:Chanayane> Capture sequence was identical to allocation index.
    return ld > rd || (ld == rd && lhs < rhs);
    // </AS:Chanayane>
}

// Detach one naturally ordered run. Reverse runs are reversed while they are
// detached, so the returned run is always in the required far-to-near order.
uint take_natural_run(inout uint current, out uint tail)
{
    uint head = current;
    tail = head;
    uint next = oitNodes[tail].next;
    if (next == OIT_NULL)
    {
        current = OIT_NULL;
        return head;
    }

    bool reverse = comes_first(next, tail);
    while (next != OIT_NULL)
    {
        bool continues = reverse ? comes_first(next, tail) : comes_first(tail, next);
        if (!continues) break;
        tail = next;
        next = oitNodes[tail].next;
    }
    current = next;
    oitNodes[tail].next = OIT_NULL;

    if (reverse)
    {
        uint previous = OIT_NULL;
        uint node = head;
        tail = head;
        while (node != OIT_NULL)
        {
            uint following = oitNodes[node].next;
            oitNodes[node].next = previous;
            previous = node;
            node = following;
        }
        head = previous;
    }
    return head;
}

uint merge_runs(uint a, uint b, out uint tail)
{
    uint head = OIT_NULL;
    tail = OIT_NULL;
    while (a != OIT_NULL || b != OIT_NULL)
    {
        uint selected;
        if (b == OIT_NULL || (a != OIT_NULL && comes_first(a, b)))
        {
            selected = a;
            a = oitNodes[a].next;
        }
        else
        {
            selected = b;
            b = oitNodes[b].next;
        }

        if (head == OIT_NULL) head = selected;
        else oitNodes[tail].next = selected;
        tail = selected;
    }
    if (tail != OIT_NULL) oitNodes[tail].next = OIT_NULL;
    return head;
}

uint natural_merge_pass(uint head, out uint output_run_count)
{
    uint current = head;
    uint new_head = OIT_NULL;
    uint new_tail = OIT_NULL;
    output_run_count = 0u;
    while (current != OIT_NULL)
    {
        uint left_tail;
        uint left = take_natural_run(current, left_tail);
        uint output_head = left;
        uint output_tail = left_tail;
        if (current != OIT_NULL)
        {
            uint right_tail;
            uint right = take_natural_run(current, right_tail);
            output_head = merge_runs(left, right, output_tail);
        }

        if (new_head == OIT_NULL) new_head = output_head;
        else oitNodes[new_tail].next = output_head;
        new_tail = output_tail;
        ++output_run_count;
    }
    return new_head;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint head = imageLoad(oitHeadPointers, pixel).r;
    vec4 dst = texelFetch(diffuseRect, pixel, 0);

    // <AS:Chanayane> The original pass 0 list traversal is replaced by exact
    // atomic counts written as each successfully allocated node is captured.

    if (oitPass == 1)
    {
        uint remaining_runs = imageLoad(oitListCounts, pixel).r;
        if (remaining_runs > 1u)
        {
            uint output_runs;
            head = natural_merge_pass(head, output_runs);
            imageStore(oitListCounts, pixel, uvec4(output_runs, 0u, 0u, 0u));
            imageStore(oitHeadPointers, pixel, uvec4(head, 0u, 0u, 0u));
        }
        frag_color = vec4(0.0);
        return;
    }

    if (head == OIT_NULL)
    {
        frag_color = dst;
        return;
    }

    uint count = 0u;
    float nearest = 1.0;
    float farthest = 0.0;
    for (uint n = head; n != OIT_NULL; n = oitNodes[n].next)
    {
        ++count;
        nearest = min(nearest, oitNodes[n].depth);
        farthest = max(farthest, oitNodes[n].depth);
    }

    if (oitDebugMode == 1)
    {
        float heat = min(float(count) / 32.0, 1.0);
        frag_color = vec4(heat, heat * heat, 1.0 - heat, 1.0);
        return;
    }
    if (oitDebugMode == 2) { frag_color = vec4(vec3(nearest), 1.0); return; }
    if (oitDebugMode == 3) { frag_color = vec4(vec3(farthest), 1.0); return; }

    if (oitDebugMode == 4)
    {
        bool invalid = false;
        float previous = 1.0;
        for (uint n = head; n != OIT_NULL; n = oitNodes[n].next)
        {
            invalid = invalid || oitNodes[n].depth > previous;
            previous = oitNodes[n].depth;
        }
        frag_color = invalid ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 0.35, 0.0, 1.0);
        return;
    }
    if (oitDebugMode == 5)
    {
        uint mode = oitNodes[head].blend;
        frag_color = mode == 0xffffffffu ? vec4(1.0, 0.5, 0.0, 1.0) :
            vec4(float(mode & 255u) / 9.0, float((mode >> 8u) & 255u) / 9.0, 0.5, 1.0);
        return;
    }
    if (oitDebugMode == 6)
    {
        float utilization = oitNodeCapacity == 0u ? 0.0 : min(float(oitNodeCount) / float(oitNodeCapacity), 1.0);
        frag_color = oitOverflow != 0u ? vec4(1.0, 0.0, 1.0, 1.0) :
            vec4(utilization, 1.0 - utilization, 0.0, 1.0);
        return;
    }
    float glow = dst.a;
    for (uint n = head; n != OIT_NULL; n = oitNodes[n].next)
    {
        OITNode node = oitNodes[n];
        if (node.blend == 0xffffffffu)
        {
            glow += node.glow;
            continue;
        }
        uint color_src = node.blend & 255u;
        uint color_dst = (node.blend >> 8u) & 255u;
        uint alpha_src = (node.blend >> 16u) & 255u;
        uint alpha_dst = (node.blend >> 24u) & 255u;
        vec4 sf = blend_factor(color_src, node.color, dst);
        vec4 df = blend_factor(color_dst, node.color, dst);
        vec4 asf = blend_factor(alpha_src, node.color, dst);
        vec4 adf = blend_factor(alpha_dst, node.color, dst);
        dst.rgb = node.color.rgb * sf.rgb + dst.rgb * df.rgb;
        dst.a = node.color.a * asf.a + dst.a * adf.a;
        glow = node.glow + glow * (1.0 - node.color.a);
    }
    dst.a = max(dst.a, glow);
    frag_color = max(dst, vec4(0.0));
}
// </AS:Chanayane>
