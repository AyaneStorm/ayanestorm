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
// Enables lossless opaque-cutoff discovery on the first sort pass only.
uniform int oitFirstSortPass;
// Reports whether compute sorting completed this frame for diagnostic mode 9.
uniform int oitComputeSortActive;
// <AS:Chanayane> Self-lighting floater isolate-background mode: pass 3 is a
// depth-only re-pass (see FSExactOIT::composite()), drawn AFTER the normal
// color blend (pass 2) completes, with color writes masked off and depth
// writes on. It discards on every pixel this shader has no real captured
// coverage for (head == OIT_NULL) and writes a near-plane depth everywhere
// else, so a later depth-tested isolate backdrop pass correctly treats
// OIT-composited pixels as "something was drawn" instead of painting over
// them -- alpha-blended content never writes depth in this pipeline by
// default (correct, ordinary alpha-blending behavior), which is exactly
// what made hair/OIT content vanish under isolate mode originally. Doing
// this as a separate discard-driven pass (rather than writing depth inline
// during the color pass) avoids ever needing to read the scene's existing
// depth while it's simultaneously bound for writing, which is undefined
// behavior. Pass 3 never runs at all unless isolate mode is active.

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

// The standard alpha tuple completely overwrites the destination
// color, alpha, and accumulated glow when the shader-produced alpha is exactly one.
bool is_opaque_cutoff(uint node)
{
    const uint standard_alpha_blend = 7u | (9u << 8u) | (1u << 16u) | (9u << 24u);
    return oitNodes[node].blend == standard_alpha_blend &&
        oitNodes[node].color.a == 1.0;
}

// Keeps the nearest qualifying cutoff and every node ordered at or in front of
// it. Retained nodes stay in their current linked-list order for the natural pass.
uint prune_behind_opaque_cutoff(uint head, out uint retained_count)
{
    uint cutoff = OIT_NULL;
    retained_count = 0u;
    for (uint node = head; node != OIT_NULL; node = oitNodes[node].next)
    {
        ++retained_count;
        if (is_opaque_cutoff(node) &&
            (cutoff == OIT_NULL || comes_first(cutoff, node)))
        {
            cutoff = node;
        }
    }

    if (cutoff == OIT_NULL)
    {
        return head;
    }

    retained_count = 0u;
    uint retained_head = OIT_NULL;
    uint retained_tail = OIT_NULL;
    for (uint node = head; node != OIT_NULL;)
    {
        uint following = oitNodes[node].next;
        if (node == cutoff || comes_first(cutoff, node))
        {
            if (retained_head == OIT_NULL) retained_head = node;
            else oitNodes[retained_tail].next = node;
            retained_tail = node;
            ++retained_count;
        }
        node = following;
    }
    oitNodes[retained_tail].next = OIT_NULL;
    return retained_head;
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

    // <AS:Chanayane> See the block comment above oitDebugMode's uniform
    // declarations: pass 3 only ever runs for isolate mode, after the
    // normal color blend (pass 2) is already complete, with color writes
    // masked off. It never reads any list-mutation state, only the
    // (already-final, untouched by pass 3) head pointer.
    if (oitPass == 3)
    {
        if (head == OIT_NULL)
        {
            discard;
        }
        gl_FragDepth = 0.0;
        return;
    }
    // </AS:Chanayane>

    // <AS:Chanayane> The original pass 0 list traversal is replaced by exact
    // atomic counts written as each successfully allocated node is captured.

    if (oitPass == 1)
    {
        uint remaining_runs = imageLoad(oitListCounts, pixel).r;
        // Discover and apply the exact opaque cutoff before sorting.
        if (oitFirstSortPass != 0 && remaining_runs > 1u)
        {
            head = prune_behind_opaque_cutoff(head, remaining_runs);
            imageStore(oitListCounts, pixel, uvec4(remaining_runs, 0u, 0u, 0u));
            imageStore(oitHeadPointers, pixel, uvec4(head, 0u, 0u, 0u));
        }
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

    vec4 dst = texelFetch(diffuseRect, pixel, 0);
    if (head == OIT_NULL)
    {
        // Screen alpha carries glow, not display opacity. Diagnostics clear it
        // so later post-processing cannot turn the visualization white.
        frag_color = oitDebugMode >= 1 && oitDebugMode <= 9 ?
            vec4(dst.rgb, 0.0) : dst;
        return;
    }

    if (oitDebugMode == 9)
    {
        frag_color = oitComputeSortActive != 0 ?
            vec4(0.0, 0.8, 0.15, 0.0) : vec4(0.9, 0.0, 0.0, 0.0);
        return;
    }

    if (oitDebugMode == 7)
    {
        uint count_before_node = 0u;
        uint hidden_behind_cutoff = 0u;
        bool cutoff_found = false;
        for (uint n = head; n != OIT_NULL; n = oitNodes[n].next)
        {
            if (is_opaque_cutoff(n))
            {
                cutoff_found = true;
                hidden_behind_cutoff = count_before_node;
            }
            ++count_before_node;
        }

        if (!cutoff_found)
        {
            frag_color = vec4(0.0);
        }
        else if (hidden_behind_cutoff == 0u)
        {
            frag_color = vec4(0.0, 0.25, 1.0, 0.0);
        }
        else
        {
            float heat = min(float(hidden_behind_cutoff) / 16.0, 1.0);
            frag_color = vec4(heat, heat * 0.5, 0.0, 0.0);
        }
        return;
    }

    // Count and depth scans are diagnostic work. Normal compositing proceeds
    // directly to blending so each visible node is read only for useful output.
    if ((oitDebugMode >= 1 && oitDebugMode <= 3) || oitDebugMode == 8)
    {
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
            frag_color = vec4(heat, heat * heat, 1.0 - heat, 0.0);
            return;
        }
        if (oitDebugMode == 2) { frag_color = vec4(vec3(nearest), 0.0); return; }
        if (oitDebugMode == 3) { frag_color = vec4(vec3(farthest), 0.0); return; }

        // Exact list-depth buckets: 1, 2-4, 5-8, 9-16, 17-32, 33-64, and 65+.
        frag_color = count == 1u  ? vec4(0.10, 0.10, 0.10, 0.0) :
                     count <= 4u  ? vec4(0.00, 0.25, 1.00, 0.0) :
                     count <= 8u  ? vec4(0.00, 0.80, 1.00, 0.0) :
                     count <= 16u ? vec4(0.00, 0.80, 0.20, 0.0) :
                     count <= 32u ? vec4(1.00, 0.90, 0.00, 0.0) :
                     count <= 64u ? vec4(1.00, 0.35, 0.00, 0.0) :
                                    vec4(1.00, 0.00, 0.75, 0.0);
        return;
    }
    if (oitDebugMode == 4)
    {
        bool invalid = false;
        float previous = 1.0;
        for (uint n = head; n != OIT_NULL; n = oitNodes[n].next)
        {
            invalid = invalid || oitNodes[n].depth > previous;
            previous = oitNodes[n].depth;
        }
        frag_color = invalid ? vec4(1.0, 0.0, 0.0, 0.0) : vec4(0.0, 0.35, 0.0, 0.0);
        return;
    }
    if (oitDebugMode == 5)
    {
        uint mode = oitNodes[head].blend;
        frag_color = mode == 0xffffffffu ? vec4(1.0, 0.5, 0.0, 0.0) :
            vec4(float(mode & 255u) / 9.0, float((mode >> 8u) & 255u) / 9.0, 0.5, 0.0);
        return;
    }
    if (oitDebugMode == 6)
    {
        float utilization = oitNodeCapacity == 0u ? 0.0 : min(float(oitNodeCount) / float(oitNodeCapacity), 1.0);
        frag_color = oitOverflow != 0u ? vec4(1.0, 0.0, 1.0, 0.0) :
            vec4(utilization, 1.0 - utilization, 0.0, 0.0);
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
