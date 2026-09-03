# AVBOIT: hair and layered objects render darker behind windows

Author: chanayane@firestorm. Date: 2026-09-04. Status: fixed and
confirmed (four exact layers, frontKey0-3, plus the relative volume
weight refinement). Long-standing bug, present since the first AVBOIT
builds; not introduced by A9 or by the 2026-09-03 tile-range work. A9
removed the same defect for the no-glass case, which is why the two
cases differed until this fix.

Three keys (frontKey0-2) were built and tested first; still darker than
Exact OIT, as expected (strands four and deeper still took the volume's
undiscriminating weight). frontKey3 and the relative volume weight are
the follow-up fix -- see "Implementation status: fourth key + relative
volume weight" below.

Searchable terms: AVBOIT glass darkening, hair dark through window, front
key, A9 third layer, avboitFrontKey2, double-sided alpha pane.

## Symptom

Seen through an alpha window pane, hair (and other objects made of many
alpha layers) renders visibly darker in AVBOIT than in vanilla or Exact
OIT. Single-layer objects behind the same pane (skin, dress, floor) match.
Without the pane in front, the same hair matches (A9, front-two-layer
key, fixed that case; see `ayanestorm-oit-performance-audit-plan.md` A9
and `ayanestorm-oit-avboit-hair-flicker-regression-todo.md` round 10).

## Cause

A9 gives exact per-pixel source-over weights to the two nearest distinct
transparent depths (`avboitFrontKey0/1`, `avboit_store_front_key()` in
`avboitCaptureF.glsl`) and leaves every deeper layer on the per-cell
volume weight, bounded by `(1 - a0)(1 - a1)`.

Hair looks right with A9 because at each pixel both exact slots are hair:
the front strand and the strand behind it. Strands three and deeper are
bounded by `(1 - a1)(1 - a2)`, small once two strands are in front, so the
volume's over-weighting of back strands cannot show.

A window pane in front takes one slot. Hair behind it keeps only one
exact strand; the second strand and everything behind it fall back to the
volume weight, bounded by `(1 - a_glass)(1 - a1)`, which is large (one
thin strand hides little). The volume cannot separate strands two, three
and four inside a cell (a thin strand covers a few of the 16
sub-samples), so they all get close to that bound and the normalized
average tilts toward the unlit back-facing strands: darker hair. This is
the pre-A9 hair defect returning one layer deeper. One glass face is
enough (confirmed: the test pane is a single face, no thickness); a thick
pane just costs one more slot. Any object that needs two exact layers of
its own shows the same behind a pane; single-layer objects do not.

Long-standing: before A9 every layer used the volume weight and the same
darkening existed with or without glass.

## Confirming test (done)

The test window is a single face and hair is still darker, so the pane
consumes one slot and the mechanism above is the operative one. No
further in-world test needed.

## Fix: four front keys (A9 extended)

Add exact slots so that a pane (one face) or a thick pane (two faces)
still leaves hair its two exact strands: four keys total. Cost: two more
full-resolution R32UI images (8 bytes/pixel) and at most two more
`imageAtomicMin` per alpha fragment in pass 3, paid only by fragments
displaced that far. Written below for `frontKey2`; `frontKey3` is the same
one level further (image unit 5, `GL_COLOR_ATTACHMENT3`, one more cascade
step, one more `else if`, one more `(1 - key_alpha3)` factor in the
bound). Image units 2 and 5 are free during the raster passes; the
resolve rebinds them after pass 2 has finished reading the keys.

### fsavboit.cpp

- `sResources.frontKey2` allocated like `frontKey0/1`
  (`allocateAccumulationTexture(..., GL_R32UI, width, height)`), attached
  to `frontKeyFBO` as `GL_COLOR_ATTACHMENT2` (fallback clear path), deleted
  in `releaseResources()`, checked in `beginDirectFrame()`'s completeness
  gate.
- `beginDirectRasterPass(3)`: clear it to `0xffffffff` with the same
  `glClearTexImage` / `glClearBufferuiv(GL_COLOR, 2, ...)` fallback, and
  `glBindImageTexture(2, sResources.frontKey2, 0, GL_FALSE, 0,
  GL_READ_WRITE, GL_R32UI)`. Image unit 2 is free during the raster
  passes (the resolve rebinds it to the screen later in the frame, after
  pass 2 has finished reading the keys).

### avboitCaptureF.glsl (and the same declarations/reads in
`avboitEmissiveF.glsl`, `avboitPbrGlowF.glsl`)

Declaration next to the other two:

```glsl
layout(binding = 2, r32ui) uniform coherent uimage2D avboitFrontKey2;   // third nearest, distinct depth
```

Insertion: extend the cascade one level. Same argument as the two-slot
version: each slot ends as the minimum of everything displaced into it,
equal-depth values never consume a slot.

```glsl
void avboit_store_front_key(float alpha)
{
    if (alpha <= 0.0)
    {
        return;
    }
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint key = avboit_front_key(alpha);
    uint previous = imageAtomicMin(avboitFrontKey0, pixel, key);
    if ((previous >> 8u) == (key >> 8u))
    {
        return;
    }
    uint displaced = key < previous ? previous : key;
    if (displaced == 0xffffffffu)
    {
        return;
    }
    previous = imageAtomicMin(avboitFrontKey1, pixel, displaced);
    if ((previous >> 8u) == (displaced >> 8u))
    {
        return;
    }
    displaced = displaced < previous ? previous : displaced;
    if (displaced == 0xffffffffu)
    {
        return;
    }
    imageAtomicMin(avboitFrontKey2, pixel, displaced);
}
```

Pass-2 weighting: add the third case and extend the bound.

```glsl
uint key2 = imageLoad(avboitFrontKey2, pixel).r;
float key_alpha2 = key2 == 0xffffffffu ? 0.0 : float(key2 & 255u) / 255.0;
...
else if (avboitFrontLayers != 0 && key2 != 0xffffffffu &&
         my_depth == (key2 >> 8u))
{
    front_factor = (1.0 - key_alpha0) * (1.0 - key_alpha1);  // third layer
}
else if (avboitFrontLayers != 0)
{
    front_factor = min(front_transmittance,
                       (1.0 - key_alpha0) * (1.0 - key_alpha1) *
                       (1.0 - key_alpha2));
}
```

Glow shaders: same read and same three cases where they compute
`front_factor`.

### Relative volume weight for deeper layers (required, second build)

Why it is needed: for every layer past the keys, `front_factor =
min(T_volume, bound)`. `T_volume` at a strand behind glass and two hair
strands is the cell-averaged extinction of the pane and of the sub-samples
of those strands, which is close to `bound` and nearly the same for
strands four, five and six, so they all get almost the full remaining
weight. The exact answer for strand `n` is `bound` times the transmittance
of strands three to `n - 1` at this pixel; the volume can only give the
cell average of that, but it must at least be *relative to the last key*
rather than to the camera, otherwise the keyed layers' cell average is
applied a second time on top of their exact product.

Implementation (`avboitCaptureF.glsl`, pass 2, tile-mode branch and
global branch alike; the glow shaders keep the plain bound, glow is not
worth the extra reads):

```glsl
// After key0..key3 and their alphas are loaded, and only in the final
// else-if (this fragment matched no key):
uint last_key = key3 != 0xffffffffu ? key3 :
                key2 != 0xffffffffu ? key2 :
                key1 != 0xffffffffu ? key1 : key0;
float bound = (1.0 - key_alpha0) * (1.0 - key_alpha1) *
              (1.0 - key_alpha2) * (1.0 - key_alpha3);
if (last_key == 0xffffffffu)
{
    front_factor = front_transmittance;      // no keys: old behaviour
}
else
{
    // Window depth of the deepest exact layer at this pixel; same
    // quantization as avboit_front_key(), so this is that surface's own
    // gl_FragCoord.z to within 6e-8.
    float key_depth = float(last_key >> 8u) / 16777215.0;
    // Volume transmittance the deepest keyed surface itself would read
    // at this pixel: same read path as this fragment's own read, same
    // bias, but no cell-centre extrapolation (dz = 0; the keyed
    // surface's slope is unknown here and it is a different primitive).
    float key_transmittance;
    if (avboitTileRange != 0)
    {
        key_transmittance = avboit_front_transmittance(
            full_res_pixel, key_depth, key_depth, 0.0, 0.0, 0.0);
    }
    else
    {
        float key_slice = avboit_warped_slice_global(key_depth);
        key_transmittance = texture(avboitTransmittanceSampler,
            vec3(sample_xy, (max(key_slice - avboitSamplingBias, 0.0) + 0.5) /
                float(AVBOIT_DIRECT_SLICES))).r;
    }
    key_transmittance = max(key_transmittance, 1.0 / 16384.0);
    // Only content between the deepest key and this fragment attenuates
    // it; the keyed layers are already applied exactly through `bound`.
    float relative = clamp(front_transmittance / key_transmittance, 0.0, 1.0);
    front_factor = bound * relative;
}
```

Notes for the implementer:

- `avboit_front_transmittance(pixel, biased_window_depth, z, dz_dx, dz_dy,
  slope_limit)` is the existing round-8 signature: pass `key_depth` for
  both depth arguments and zeros for the three derivative arguments.
  Zero derivatives are legal input; the function only multiplies by them.
- `front_transmittance` above is the value already computed for this
  fragment (after the floor). Do not recompute it.
- `bound * relative` replaces `min(front_transmittance, bound)`; with no
  keys the branch falls back to the old value, so
  `RenderAVBOITFrontLayers = 0` behaviour is unchanged.
- Cost: one extra 4-cell read for fragments that matched no key only.

### Fourth key (required, same build)

Do it now, not "if reported": a pane with thickness, a pane plus a
curtain, glasses lenses plus hair, all take two slots before the hair.
Pattern is exactly the one used for `frontKey2`:

- `Resources::frontKey3`, allocated `GL_R32UI`, attached as
  `GL_COLOR_ATTACHMENT3` on `frontKeyFBO`, cleared in
  `beginDirectRasterPass(3)` on both clear paths, released, completeness
  gate. Bound to image unit 5 (`glBindImageTexture(5, ...)`). Unit 5 is
  used only by the resolve (`accumulatedExtinction`, bound in
  `finishDirectFrame()` after pass 2); no conflict, same situation as
  unit 2.
- Shaders (`avboitCaptureF.glsl`, both glow shaders): `layout(binding = 5,
  r32ui) uniform coherent uimage2D avboitFrontKey3;`. In
  `avboit_store_front_key()`, replace the final unconditional
  `imageAtomicMin(avboitFrontKey2, ...)` with the same three-line step used
  for Key1 (atomicMin into Key2, return on equal depth, take the larger,
  return on sentinel) and then `imageAtomicMin(avboitFrontKey3, pixel,
  displaced)`. In pass 2: load `key3`, `key_alpha3`, add
  `else if (my_depth == (key3 >> 8u)) front_factor = (1 - a0)(1 - a1)(1 - a2);`
  and the four-factor `bound` shown above.

## Implementation status (2026-09-04)

`frontKey2` implemented exactly as specified above (frontKey3 deliberately
skipped, user decision -- not confirmed necessary, add later using the
same one-more-cascade-level pattern this implementation used to extend
frontKey0/1 to frontKey2, if a thick two-face pane case is ever reported).

- `fsavboit.h`: `Resources::frontKey2` added next to `frontKey0`/`frontKey1`.
- `fsavboit.cpp`: `frontKey2` allocated (`GL_R32UI`) in `allocateVolume()`,
  attached to `frontKeyFBO` as `GL_COLOR_ATTACHMENT2`, deleted in
  `releaseResources()`, checked in `beginDirectFrame()`'s completeness
  gate, cleared to `0xffffffffu` in `beginDirectRasterPass(3)` (both the
  `glClearTexImage` and `glClearBufferuiv` fallback paths), bound to image
  unit 2 (`GL_READ_WRITE`) alongside units 0/1. Confirmed image unit 2 is
  free until `finishDirectFrame()`'s resolve rebinds it to `screen`
  (fsavboit.cpp:2081, well after pass 2 has read the key) -- no conflict.
- `avboitCaptureF.glsl`: `avboitFrontKey2` declared (binding 2).
  `avboit_store_front_key()` extended one more cascade level exactly per
  the doc's listing (displaced value from Key1's atomicMin, if any and
  distinct, goes to Key2). Pass-2 weighting gets the third `else if`
  (`front_factor = (1 - key_alpha0) * (1 - key_alpha1)` when this
  fragment's depth matches key2) and the fallback bound gains the
  `(1 - key_alpha2)` factor.
- `avboitEmissiveF.glsl`, `avboitPbrGlowF.glsl`: identical declaration and
  cascade addition in their own `front_factor` computations.
- Optional relative-volume-weight refinement (doc's "Optional, same
  build" section): not implemented -- doc calls it secondary, the third
  key alone fixes the reported case.

Not yet built or tested. Verification below is unchanged from the
original spec.

## Verification

1. Hair through the test pane matches Exact OIT (side by side).
2. Hair without a pane unchanged from the A9 result.
3. Dress over under-garment unchanged (keys 0/1 already covered it).
4. `RenderAVBOITFrontLayers = 0` still gives the old behaviour.
5. Cost: pass 3 time before/after in the GPU profiler zone; expect
   negligible.

## If hair behind glass is still darker with four keys

Add a debug mode that colours pixels by the number of filled keys (0 to
4) and by which key this pixel's front hair strand matched, and report
that before any further change. The remaining suspect would then be the
deeper-layer bound leaking through soft strand edges (alpha 0.3 to 0.9),
for which the optional relative volume weight above is the next step.

## Status after the first glass build (2026-09-04)

Three keys built and tested: hair behind the pane "a bit better", still
darker than Exact OIT. Expected: two exact hair strands behind one pane
fix the largest term; strands four and deeper still take the volume's
undiscriminating weight. Next build: the fourth key and the relative
volume weight above, both now specified as code.

## Implementation status: fourth key + relative volume weight (2026-09-04)

Both implemented exactly as specified above.

- `fsavboit.h`: `Resources::frontKey3` added next to `frontKey2`.
- `fsavboit.cpp`: `frontKey3` allocated (`GL_R32UI`), attached to
  `frontKeyFBO` as `GL_COLOR_ATTACHMENT3`, deleted, gated, cleared on both
  paths, bound to image unit 5 in `beginDirectRasterPass(3)`. Confirmed
  unit 5 is otherwise touched only by `finishDirectFrame()`'s resolve
  (`accumulatedExtinction`, fsavboit.cpp:2087), after pass 2 -- no
  conflict, same situation as unit 2 with `screen`.
- `avboitCaptureF.glsl`: `avboitFrontKey3` declared (binding 5).
  `avboit_store_front_key()` extended one more cascade level (displaced
  value from Key2's atomicMin goes to Key3, same equal-depth/sentinel
  short-circuits as every other level). Pass-2 weighting gets the fourth
  `else if` (front_factor = product of the first three `1 - key_alpha`
  terms) and the final fallback branch now computes `last_key`/`bound`
  over all four keys and applies the relative volume weight: reads the
  volume once more at the deepest key's own depth (through
  `avboit_front_transmittance()` in tile mode, the direct
  `avboitTransmittanceSampler` read with the existing sampling-bias
  offset in global mode), floors it the same way as `front_transmittance`,
  and uses `bound * clamp(front_transmittance / key_transmittance, 0, 1)`
  instead of `min(front_transmittance, bound)`. `sample_xy` in the global
  branch is recomputed locally (`key_sample_xy`) rather than reusing the
  outer one, which is scoped to the transmittance-sampling `else` block
  above and not visible at the front_factor site.
- `avboitEmissiveF.glsl`, `avboitPbrGlowF.glsl`: same declaration and
  fourth-key case; the fallback keeps the plain four-factor `bound`
  without the relative-volume-weight read, per the doc's "glow is not
  worth the extra reads" note.

Verified: `avboit_front_transmittance()` call matches its declared
signature exactly (pixel, biased depth, z, dz_dx, dz_dy, slope_limit;
zero derivatives are legal, the function only multiplies by them).
`full_res_pixel` is in scope at the call site (declared earlier in pass
2, used unconditionally). Brace balance checked on all 5 touched files.

## Status after the second glass build (2026-09-04): fixed

Four keys plus the relative volume weight, built and tested: hair through
the window pane now looks good. Bug closed.
