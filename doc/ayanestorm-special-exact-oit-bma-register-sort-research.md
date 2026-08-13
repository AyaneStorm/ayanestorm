# Exact OIT BMA and register-sort research

## Primary references

- P. Knowles, G. Leach, and F. Zambetta, *Backwards Memory Allocation and
  Improved OIT*, Pacific Graphics 2013:
  https://diglib.eg.org/items/b0612bfa-918d-437b-8332-a1f2d77c3753
- P. Knowles, G. Leach, and F. Zambetta, *Fast Sorting for Exact OIT of Complex
  Scenes*, CGI 2014:
  https://www.heuristic42.com/dl/rbs-preprint.pdf

Both techniques preserve exact per-fragment ordering.

## Backwards memory allocation

The BMA paper targets an exact-OIT implementation that copies every pixel's
fragments into a shader-local array and sorts them there. A single shader
normally declares an array large enough for the worst supported depth
complexity. That large static allocation reduces the number of shader
invocations that can reside on a GPU execution unit, even when most pixels have
short lists.

BMA classifies pixels into power-of-two depth-complexity ranges and processes
each range with a shader variant whose statically allocated local array is
within a factor of two of the required size. The smaller variants improve GPU
occupancy for the common shallow pixels. The paper reports up to a threefold
overall OIT speedup for its implementation and test scenes.

## Register-based block sorting

Register-based block sort loads a small block of fragment sort keys into
registers, sorts that block using fixed register operations, writes sorted
blocks to slower storage, and merges the blocks. It applies the GPU memory
hierarchy in the style of an external merge sort. The associated research
reports sorting as 70--95% of OIT time in its deep test scenes and reports up to
1.7 times the performance of its previous best BMA method, or 6.3 times a
straightforward baseline.

These are historical results on different hardware, shaders, storage layouts,
scenes, and APIs. They demonstrate the value of specialization and registers;
they do not predict the same multiplier in AyaneStorm.

## Relationship to the AyaneStorm implementation

AyaneStorm currently does not allocate a worst-case shader-local fragment
array. It stores 32-byte nodes in a global SSBO and sorts each per-pixel linked
list by rewiring `next` indices during fullscreen natural merge passes.
Consequently, the specific local-array occupancy problem solved by BMA is not
present, and BMA cannot simply be applied to the current shader for an expected
threefold improvement.

The transferable ideas are nevertheless relevant:

1. Classify work by exact list depth rather than running one generalized path.
2. Use fixed-size, compiler-visible shader variants for shallow lists.
3. Sort small blocks in registers and merge only when the list exceeds the
   block size.
4. Keep the existing global linked-list merge as a deep-list fallback.

A possible exact hybrid would provide unrolled variants for list capacities
such as 4, 8, 16, and perhaps 32. Each variant would load node indices and
depths, apply a fixed sorting network in registers, and relink the nodes in
sorted order. Longer lists would use register-sorted blocks followed by global
merges, or retain the current natural-run algorithm.

## Integration risks

Efficiently dispatching only the pixels in each depth bucket requires pixel
compaction or indirect work generation. The previous AyaneStorm active-pixel
sorting experiment caused repeatable driver crashes even after correcting its
vertex-fetch bounds and was removed. BMA-style scheduling must therefore not
reuse that unproven graphics point-draw path.

Dynamic indexing of GLSL arrays may also cause register data to spill into
thread-local memory, defeating the purpose. Fixed, unrolled compare/exchange
networks and inspection with vendor shader tools would be required.

Without safe compact dispatch, a fullscreen pass per bucket or shader variant
could cost more than it saves. A compute-shader implementation would be the
natural modern design, but would introduce a new execution path and require
careful OpenGL capability and driver validation.

## Recommendation

BMA itself is not a direct optimization for the current global linked-list
sort. Register sorting is a promising exact replacement for short-list sorting,
provided a safe scheduling mechanism can be established. Before implementation,
collect a histogram of per-pixel list depths and GPU time by list range. That
will show how much work lies in capacities 1--4, 5--8, 9--16, 17--32, and above
32, and therefore whether specialized variants could affect enough pixels to
justify their dispatch and maintenance cost.

Shader-cache revision v14 includes diagnostic mode 8 as the first instrumentation
step. It displays exact per-pixel depth buckets for 1, 2--4, 5--8, 9--16,
17--32, 33--64, and 65-or-more fragments. This is a visual distribution rather
than a timed or numeric GPU histogram, but it requires no new SSBO layout,
readback, compaction, or dispatch mechanism and is therefore suitable for the
initial runtime decision. The separate Highlight Transparent overlay is
suppressed while Exact OIT diagnostics are active so it cannot obscure the
bucket colors.
