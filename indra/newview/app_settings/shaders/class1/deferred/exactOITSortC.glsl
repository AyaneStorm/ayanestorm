/**
 * Exact OIT compute classification, shared-memory block sorting, and queued merging.
 * Each permutation operates on the existing lossless linked-list nodes.
 */

layout(binding = 0, r32ui) uniform coherent uimage2D oitHeadPointers;
layout(binding = 1, r32ui) uniform coherent uimage2D oitListCounts;

struct OITNode
{
    vec4 color;
    float glow;
    float depth;
    uint next;
    uint blend;
};

layout(std430, binding = 0) buffer OITNodes
{
    OITNode oitNodes[];
};

layout(std430, binding = 2) buffer OITInputQueue
{
    uint inputDispatchX;
    uint inputDispatchY;
    uint inputDispatchZ;
    uint inputPad;
    uint inputPixels[];
};

layout(std430, binding = 3) buffer OITOutputQueue
{
    uint outputDispatchX;
    uint outputDispatchY;
    uint outputDispatchZ;
    uint outputPad;
    uint outputPixels[];
};

const uint OIT_NULL = 0xffffffffu;

bool comes_first(uint lhs, uint rhs)
{
    if (lhs == OIT_NULL) return false;
    if (rhs == OIT_NULL) return true;
    float ld = oitNodes[lhs].depth;
    float rd = oitNodes[rhs].depth;
    return ld > rd || (ld == rd && lhs < rhs);
}

uint pack_pixel(ivec2 pixel)
{
    return uint(pixel.x) | (uint(pixel.y) << 16u);
}

ivec2 unpack_pixel(uint packed_pixel)
{
    return ivec2(int(packed_pixel & 0xffffu), int(packed_pixel >> 16u));
}

void append_output(uint packed_pixel)
{
    uint output_index = atomicAdd(outputDispatchX, 1u);
    outputPixels[output_index] = packed_pixel;
}

#ifdef OIT_CLASSIFY

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(pixel, imageSize(oitListCounts))))
    {
        return;
    }

    if (imageLoad(oitListCounts, pixel).r > 1u)
    {
        append_output(pack_pixel(pixel));
    }
}

#endif

#ifdef OIT_BLOCK_SORT

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

uniform int oitOpaqueCutoff;

shared uint block_nodes[64];
shared uint current_node;
shared uint previous_tail;
shared uint block_count;
shared uint run_count;
shared uint list_head;
shared uint list_count;
shared uint packed_pixel;

bool is_opaque_cutoff(uint node)
{
    const uint standard_alpha_blend = 7u | (9u << 8u) | (1u << 16u) | (9u << 24u);
    return oitNodes[node].blend == standard_alpha_blend &&
        oitNodes[node].color.a == 1.0;
}

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

void main()
{
    uint lane = gl_LocalInvocationID.x;
    if (lane == 0u)
    {
        packed_pixel = inputPixels[gl_WorkGroupID.x];
        ivec2 pixel = unpack_pixel(packed_pixel);
        list_head = imageLoad(oitHeadPointers, pixel).r;
        list_count = imageLoad(oitListCounts, pixel).r;
        if (oitOpaqueCutoff != 0 && list_count > 1u)
        {
            list_head = prune_behind_opaque_cutoff(list_head, list_count);
        }
        current_node = list_head;
        previous_tail = OIT_NULL;
        run_count = 0u;
    }
    barrier();

    while (current_node != OIT_NULL)
    {
        if (lane == 0u)
        {
            uint node = current_node;
            block_count = 0u;
            while (node != OIT_NULL && block_count < 64u)
            {
                block_nodes[block_count++] = node;
                node = oitNodes[node].next;
            }
            current_node = node;
        }
        barrier();

        if (lane >= block_count)
        {
            block_nodes[lane] = OIT_NULL;
        }
        barrier();

        for (uint width = 2u; width <= 64u; width <<= 1u)
        {
            for (uint stride = width >> 1u; stride > 0u; stride >>= 1u)
            {
                uint partner = lane ^ stride;
                if (partner > lane)
                {
                    uint a = block_nodes[lane];
                    uint b = block_nodes[partner];
                    bool ascending = (lane & width) == 0u;
                    bool swap_nodes = ascending ? comes_first(b, a) : comes_first(a, b);
                    if (swap_nodes)
                    {
                        block_nodes[lane] = b;
                        block_nodes[partner] = a;
                    }
                }
                barrier();
            }
        }

        if (lane < block_count)
        {
            oitNodes[block_nodes[lane]].next =
                lane + 1u < block_count ? block_nodes[lane + 1u] : OIT_NULL;
        }
        barrier();

        if (lane == 0u)
        {
            if (previous_tail == OIT_NULL) list_head = block_nodes[0];
            else oitNodes[previous_tail].next = block_nodes[0];
            previous_tail = block_nodes[block_count - 1u];
            ++run_count;
        }
        barrier();
    }

    if (lane == 0u)
    {
        ivec2 pixel = unpack_pixel(packed_pixel);
        imageStore(oitHeadPointers, pixel, uvec4(list_head, 0u, 0u, 0u));
        imageStore(oitListCounts, pixel, uvec4(run_count, 0u, 0u, 0u));
        if (run_count > 1u)
        {
            append_output(packed_pixel);
        }
    }
}

#endif

#ifdef OIT_MERGE

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

uint take_natural_run(inout uint current, out uint tail)
{
    uint head = current;
    tail = head;
    uint next = oitNodes[tail].next;
    while (next != OIT_NULL && comes_first(tail, next))
    {
        tail = next;
        next = oitNodes[tail].next;
    }
    current = next;
    oitNodes[tail].next = OIT_NULL;
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
    oitNodes[tail].next = OIT_NULL;
    return head;
}

uint merge_pass(uint head, out uint output_runs)
{
    uint current = head;
    uint new_head = OIT_NULL;
    uint new_tail = OIT_NULL;
    output_runs = 0u;
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
        ++output_runs;
    }
    return new_head;
}

void main()
{
    uint pixel_key = inputPixels[gl_WorkGroupID.x];
    ivec2 pixel = unpack_pixel(pixel_key);
    uint output_runs;
    uint head = merge_pass(imageLoad(oitHeadPointers, pixel).r, output_runs);
    imageStore(oitHeadPointers, pixel, uvec4(head, 0u, 0u, 0u));
    imageStore(oitListCounts, pixel, uvec4(output_runs, 0u, 0u, 0u));
    if (output_runs > 1u)
    {
        append_output(pixel_key);
    }
}

#endif
