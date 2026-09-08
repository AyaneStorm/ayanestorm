// AyaneStorm OIT shader. Author: chanayane@firestorm.
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
// <AS:Chanayane> Shallow-list fast path (E5): pixels with count <= K (in
// normal mode; forced to 0, i.e. disabled, in any debug mode so every
// diagnostic keeps seeing the fully-sorted path) are sorted and blended in
// registers by blend_shallow() below, with no fullscreen sort pass at all.
uniform int oitShallowLimit;
uniform int oitOpaqueCutoff;
const uint OIT_SORTED = 0x80000000u;
const uint OIT_SHALLOW = 16u;   // K. Must match ASExactOIT::composite()'s K.
// </AS:Chanayane>
// <AS:Chanayane> Self-lighting floater isolate-background mode: pass 3 is a
// depth-only re-pass (see ASExactOIT::composite()), drawn AFTER the normal
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
// <AS:Chanayane> Hard ceiling on every linked-list traversal below. The list
// is built lock-free by concurrent fragment shaders (imageAtomicExchange);
// it is expected to be an acyclic chain of at most oitNodeCapacity nodes,
// but nothing here can prove that at GPU-compile time. If any bug --
// current or future, in this file or the capture shaders -- ever lets a
// node's `next` form a cycle or otherwise violates that assumption, an
// unbounded `for (node = head; node != OIT_NULL; node = oitNodes[node].next)`
// becomes a true GPU-side infinite loop: unlike a CPU hang this cannot be
// timed out or recovered from the host side, and is exactly what a
// Display:RenderFlush watchdog / driver TDR hang looks like. Every traversal
// below is capped at oitNodeCapacity (not a compile-time constant, so used
// directly at each guard rather than aliased) -- a correct upper bound on
// any real list's length regardless of how it was built.
// </AS:Chanayane>

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

// <AS:Chanayane> Shallow-list fast path (E5).
// Same total order as comes_first(): greater depth first, lower index first on ties.
bool before(float da, uint ia, float db, uint ib)
{
    return da > db || (da == db && ia < ib);
}

// Blend one node over dst/glow. Extracted verbatim from the main loop so the
// shallow and sorted-list paths share one implementation.
void blend_node(uint n, inout vec4 dst, inout float glow)
{
    OITNode node = oitNodes[n];
    if (node.blend == 0xffffffffu) { glow += node.glow; return; }
    uint color_src = node.blend & 255u;
    uint color_dst = (node.blend >> 8u) & 255u;
    uint alpha_src = (node.blend >> 16u) & 255u;
    uint alpha_dst = (node.blend >> 24u) & 255u;
    vec4 sf  = blend_factor(color_src, node.color, dst);
    vec4 df  = blend_factor(color_dst, node.color, dst);
    vec4 asf = blend_factor(alpha_src, node.color, dst);
    vec4 adf = blend_factor(alpha_dst, node.color, dst);
    dst.rgb = node.color.rgb * sf.rgb + dst.rgb * df.rgb;
    dst.a   = node.color.a * asf.a + dst.a * adf.a;
    glow    = node.glow + glow * (1.0 - node.color.a);
}

// Shallow path: count <= OIT_SHALLOW, list unsorted. Gathers up to
// OIT_SHALLOW (index, depth) pairs, insertion-sorts them far-to-near in
// registers, applies the opaque cutoff, then blends -- no linked-list
// rewrite, no fullscreen sort pass.
void blend_shallow(uint head, uint count, inout vec4 dst, inout float glow)
{
    uint  idx[OIT_SHALLOW];
    float dep[OIT_SHALLOW];
    uint n = 0u;
    for (uint node = head; node != OIT_NULL && n < OIT_SHALLOW; node = oitNodes[node].next)
    {
        idx[n] = node; dep[n] = oitNodes[node].depth; ++n;
    }
    for (uint i = 1u; i < n; ++i)
    {
        uint  ki = idx[i]; float kd = dep[i]; uint j = i;
        while (j > 0u && before(kd, ki, dep[j - 1u], idx[j - 1u]))
        {
            idx[j] = idx[j - 1u]; dep[j] = dep[j - 1u]; --j;
        }
        idx[j] = ki; dep[j] = kd;
    }
    // Exact opaque cutoff: nearest qualifying node is the last one in far-to-near order.
    uint start = 0u;
    if (oitOpaqueCutoff != 0)
        for (uint i = 0u; i < n; ++i) if (is_opaque_cutoff(idx[i])) start = i;
    for (uint i = start; i < n; ++i) blend_node(idx[i], dst, glow);
}
// </AS:Chanayane>

// Keeps the nearest qualifying cutoff and every node ordered at or in front of
// it. Retained nodes stay in their current linked-list order for the natural pass.
uint prune_behind_opaque_cutoff(uint head, out uint retained_count)
{
    uint cutoff = OIT_NULL;
    retained_count = 0u;
    // <AS:Chanayane> Iteration-capped traversal: see the OIT_NULL-adjacent
    // comment above. A cyclic or otherwise corrupted list must terminate
    // this loop by cap, never spin the GPU.
    for (uint node = head, guard = 0u; node != OIT_NULL && guard < oitNodeCapacity; node = oitNodes[node].next, ++guard)
    // </AS:Chanayane>
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
    // <AS:Chanayane> Same iteration cap as above.
    for (uint node = head, guard = 0u; node != OIT_NULL && guard < oitNodeCapacity; ++guard)
    // </AS:Chanayane>
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
    if (retained_tail != OIT_NULL) oitNodes[retained_tail].next = OIT_NULL;
    return retained_head;
}
// <AS:Chanayane> E6: chunked register sort. Detaches one run starting at
// `current`: a long (>= OIT_CHUNK) or final natural run is taken as before
// (detached, reversed in place if needed); a short run is instead gathered
// into registers (up to OIT_CHUNK nodes) and insertion-sorted there, cutting
// the natural-run count (and so the merge pass count) for random-order deep
// lists such as overlapping sprites.
const uint OIT_CHUNK = 16u;   // may equal OIT_SHALLOW. Must match ASExactOIT's OIT_CHUNK.

uint take_run(inout uint current, out uint tail)
{
    // 1. Measure the natural run starting at `current` (either direction).
    uint head = current;
    uint prev = head;
    uint next = oitNodes[head].next;
    uint length = 1u;
    bool reverse = false;
    // <AS:Chanayane> Iteration-capped: see the OIT_NULL-adjacent comment in
    // this file. length already caps this loop's useful work at OIT_CHUNK,
    // but comes_first() ordering alone does not guarantee termination on a
    // corrupted/cyclic list, so guard it explicitly too.
    if (next != OIT_NULL)
    {
        reverse = comes_first(next, prev);
        for (uint guard = 0u; next != OIT_NULL && guard < oitNodeCapacity; ++guard)
        {
            bool continues = reverse ? comes_first(next, prev) : comes_first(prev, next);
            if (!continues) break;
            prev = next; next = oitNodes[prev].next; ++length;
        }
    }
    // </AS:Chanayane>
    if (length >= OIT_CHUNK || next == OIT_NULL)
    {
        // Long (or final) natural run: detach, reverse in place if needed. Same as today.
        current = next;
        oitNodes[prev].next = OIT_NULL;
        tail = prev;
        if (reverse)
        {
            // <AS:Chanayane> Iteration-capped: see the OIT_NULL-adjacent comment.
            uint p = OIT_NULL, node = head; tail = head;
            for (uint guard = 0u; node != OIT_NULL && guard < oitNodeCapacity; ++guard)
            { uint f = oitNodes[node].next; oitNodes[node].next = p; p = node; node = f; }
            // </AS:Chanayane>
            head = p;
        }
        return head;
    }
    // 2. Short run: gather up to OIT_CHUNK nodes from `head` and sort them in registers.
    uint  idx[OIT_CHUNK]; float dep[OIT_CHUNK]; uint nxt[OIT_CHUNK];
    uint n = 0u; uint node = head;
    while (node != OIT_NULL && n < OIT_CHUNK)
    {
        idx[n] = node; dep[n] = oitNodes[node].depth; nxt[n] = oitNodes[node].next;
        node = nxt[n]; ++n;
    }
    current = node;
    for (uint i = 1u; i < n; ++i)
    {
        uint ki = idx[i]; float kd = dep[i]; uint kn = nxt[i]; uint j = i;
        while (j > 0u && before(kd, ki, dep[j - 1u], idx[j - 1u]))
        { idx[j] = idx[j - 1u]; dep[j] = dep[j - 1u]; nxt[j] = nxt[j - 1u]; --j; }
        idx[j] = ki; dep[j] = kd; nxt[j] = kn;
    }
    for (uint i = 0u; i + 1u < n; ++i)
        if (nxt[i] != idx[i + 1u]) oitNodes[idx[i]].next = idx[i + 1u];   // write only when changed
    if (nxt[n - 1u] != OIT_NULL) oitNodes[idx[n - 1u]].next = OIT_NULL;
    tail = idx[n - 1u];
    return idx[0];
}
// </AS:Chanayane>

uint merge_runs(uint a, uint b, out uint tail)
{
    uint head = OIT_NULL;
    tail = OIT_NULL;
    // <AS:Chanayane> Iteration-capped: see the OIT_NULL-adjacent comment.
    for (uint guard = 0u; (a != OIT_NULL || b != OIT_NULL) && guard < oitNodeCapacity; ++guard)
    // </AS:Chanayane>
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
    // <AS:Chanayane> Iteration-capped: see the OIT_NULL-adjacent comment.
    // Each iteration consumes at least one node via take_run(), so this
    // terminates naturally well under oitNodeCapacity rounds on a correct
    // list; the cap only matters if that assumption is ever violated.
    for (uint guard = 0u; current != OIT_NULL && guard < oitNodeCapacity; ++guard)
    // </AS:Chanayane>
    {
        uint left_tail;
        uint left = take_run(current, left_tail);
        uint output_head = left;
        uint output_tail = left_tail;
        if (current != OIT_NULL)
        {
            uint right_tail;
            uint right = take_run(current, right_tail);
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
        // <AS:Chanayane> E5 shallow-list fast path. The CPU issues this same
        // oitPass == 1 branch once per merge round (oitFirstSortPass is true
        // only on the very first round, width == 1); later rounds only exist
        // for pixels the first round already flagged OIT_SORTED.
        uint raw = imageLoad(oitListCounts, pixel).r;
        if (oitFirstSortPass != 0)
        {
            // First round: pixels whose raw count is <= max(K, 1) -- so
            // single-node pixels always, plus every pixel with count <= K in
            // normal mode -- are left untouched here (raw count, no
            // OIT_SORTED flag) and are sorted/blended in registers by
            // blend_shallow() in pass 2 instead. oitShallowLimit is 0 in any
            // debug mode, which still leaves single-node pixels on this path
            // (max(0, 1) == 1) while restoring the pre-E5 "> 1u" gate for
            // everything else, so every count-based diagnostic keeps seeing
            // the fully-sorted path except the always-trivial single-node case.
            uint shallow_limit = max(uint(oitShallowLimit), 1u);
            if (raw <= shallow_limit)
            {
                frag_color = vec4(0.0);
                return;
            }
            // Discover and apply the exact opaque cutoff before sorting.
            uint remaining_runs = raw;
            head = prune_behind_opaque_cutoff(head, remaining_runs);
            uint output_runs;
            head = natural_merge_pass(head, output_runs);
            imageStore(oitListCounts, pixel, uvec4(output_runs | OIT_SORTED, 0u, 0u, 0u));
            imageStore(oitHeadPointers, pixel, uvec4(head, 0u, 0u, 0u));
            frag_color = vec4(0.0);
            return;
        }

        // Later rounds: only pixels already flagged OIT_SORTED by the first
        // round ever reach this shader with more than one run left; skip
        // everything else (already fully sorted, or never entered pass 1 at
        // all -- the shallow pixels above).
        if ((raw & OIT_SORTED) == 0u)
        {
            frag_color = vec4(0.0);
            return;
        }
        uint remaining_runs = raw & ~OIT_SORTED;
        if (remaining_runs > 1u)
        {
            uint output_runs;
            head = natural_merge_pass(head, output_runs);
            imageStore(oitListCounts, pixel, uvec4(output_runs | OIT_SORTED, 0u, 0u, 0u));
            imageStore(oitHeadPointers, pixel, uvec4(head, 0u, 0u, 0u));
        }
        frag_color = vec4(0.0);
        return;
    }

    vec4 dst = texelFetch(diffuseRect, pixel, 0);
    if (head == OIT_NULL)
    {
        if (oitDebugMode >= 1 && oitDebugMode <= 8)
        {
            // Screen alpha carries glow, not display opacity. Diagnostics
            // clear it so later post-processing cannot turn the
            // visualization white.
            frag_color = vec4(dst.rgb, 0.0);
        }
        else
        {
            // <AS:Chanayane> E5: the screen already holds the correct opaque
            // value at this pixel; discard instead of rewriting it verbatim.
            discard;
            // </AS:Chanayane>
        }
        return;
    }

    if (oitDebugMode == 7)
    {
        uint count_before_node = 0u;
        uint hidden_behind_cutoff = 0u;
        bool cutoff_found = false;
        // <AS:Chanayane> Iteration-capped: see the OIT_NULL-adjacent comment.
        for (uint n = head, guard = 0u; n != OIT_NULL && guard < oitNodeCapacity; n = oitNodes[n].next, ++guard)
        // </AS:Chanayane>
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
        // <AS:Chanayane> Iteration-capped: see the OIT_NULL-adjacent comment.
        for (uint n = head, guard = 0u; n != OIT_NULL && guard < oitNodeCapacity; n = oitNodes[n].next, ++guard)
        // </AS:Chanayane>
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
        // <AS:Chanayane> Iteration-capped: see the OIT_NULL-adjacent comment.
        for (uint n = head, guard = 0u; n != OIT_NULL && guard < oitNodeCapacity; n = oitNodes[n].next, ++guard)
        // </AS:Chanayane>
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
    // <AS:Chanayane> E5: dispatch on whether pass 1 sorted this pixel's list.
    // Sorted pixels (count was > K) traverse the already-sorted linked list,
    // same as before E5. Everything else (count <= K, including the
    // always-trivial single-node case) was left unsorted by pass 1 and is
    // sorted and blended in registers here instead.
    uint count = imageLoad(oitListCounts, pixel).r;
    float glow = dst.a;
    if ((count & OIT_SORTED) != 0u)
    {
        // <AS:Chanayane> Iteration-capped: see the OIT_NULL-adjacent comment.
        for (uint n = head, guard = 0u; n != OIT_NULL && guard < oitNodeCapacity; n = oitNodes[n].next, ++guard)
        // </AS:Chanayane>
        {
            blend_node(n, dst, glow);
        }
    }
    else if (count <= max(uint(oitShallowLimit), 1u))
    {
        blend_shallow(head, count, dst, glow);
    }
    else
    {
        // Should be unreachable if pass 1 issues a merge round whenever
        // maximum_list > K (composite()'s own gate). Reported to have fired
        // in the field (magenta screen corruption), so treat it as reachable
        // in practice rather than trusting the invariant: fall back to the
        // shallow path with the list truncated to OIT_SHALLOW nodes in link
        // order (unsorted tail dropped) instead of visibly corrupting the
        // frame. oitOverflow (debug mode 6) remains the way to see this
        // happening; it no longer paints the live scene.
        blend_shallow(head, OIT_SHALLOW, dst, glow);
    }
    dst.a = max(dst.a, glow);
    frag_color = max(dst, vec4(0.0));
}
// </AS:Chanayane>
