# AVBOIT: hair flicker regression (TODO, not yet investigated)

Author: chanayane@firestorm. Date: 2026-09-03.

## Symptom

Zooming in on part of the avatar's hair in AVBOIT shows visible flicker
(the shading changes noticeably frame to frame at a fixed camera/avatar
pose, on part of the hair). User confirmed with a pre-session viewer build
that the flicker is not present there: this is a regression introduced by
this session's AVBOIT work, not a pre-existing issue. User confirmed the
flicker was already present on a build predating A7, clearing **A7** as a
cause (consistent with A7 only touching the final opaque-colour read in
the resolve shader, strictly downstream of what the flickering debug modes
sample — see below). Remaining candidates: A1, A2, A3 (A6 already cleared
by mode 15's stability, see below).

Debug mode 14 (sampled front transmittance, see `avboitCaptureF.glsl`
around `avboit_direct_store()`) flickers at the same location. Mode 12
(banded `avboitAccumulatedWeight` reading, `avboitVolumeC.glsl`) also
flickers there. Mode 15 (chosen compaction divider) stays a steady magenta
at the same spot across frames.

Both flickering diagnostics read data produced during raster **pass 2**
(the weighted-color/extinction raster): mode 12 reads
`avboitAccumulatedWeight` directly with no volume sampling involved at all;
mode 14 reads `front_transmittance`, sampled from the 3D transmittance
volume inside pass 2's `avboit_direct_store()`. Mode 15's stability rules
out A6's compaction search picking a different divider frame to frame at
this spot — the warp reparameterization is not the cause. The two flickering
modes together point at either (a) what pass 2 itself writes into
`avboitAccumulatedWeight`/the transmittance volume varying frame to frame,
or (b) the transmittance volume's *contents* (built earlier, in compute
passes 5-6, from pass 1's extinction raster) varying frame to frame even
with a stable divider — both still consistent with A1, A2, or A3 (not A6,
now de-scoped; not A7, which is strictly downstream of pass 2/mode 12/14
and could not affect either).

## Visual evidence (2026-09-03 screenshots, debug mode 12)

Two consecutive mode-12 captures at the same pinned pose, top of the head:
the first shows continuous blue/green banded coverage (`weight` roughly
0.4-3, "meaningful coverage") across the whole hair mass at the crown; the
second, taken moments later, shows two distinct grey blobs punched into
that same region. Grey in mode 12 means `weight < 0.1`, i.e. "no meaningful
coverage" — not a re-weighted or recoloured value, but accumulated weight
collapsing to near zero for those pixels on some frames and not others.
This is coverage being intermittently dropped entirely, which favours a
per-frame-unstable *test* (something that sometimes accepts and sometimes
rejects the same hair fragments) over a purely arithmetic instability in
how a stably-admitted set of fragments gets weighted. That sharpens the
early_fragment_tests / cell-depth-prepass theory below.

## Suspected cause (not verified)

Not yet investigated further than the above. Candidates, revised after
ruling out A6's divider selection and after the mode-12 screenshots (see
"Visual evidence" above, which favours a coverage-rejection theory over a
pure re-weighting theory):

- **A2's cell-depth prepass** (leading candidate after the screenshots): if
  `gAVBOITCellDepthTarget`'s per-cell farthest-opaque-depth bake is itself
  unstable (e.g. depends on draw order within a cell, or interacts with
  alpha-tested hair strands whose exact set of contributing fragments
  varies slightly per frame due to LOD/anim), the extinction raster's
  (pass 1) early_fragment_tests could admit or reject a slightly different
  set of hair fragments each frame — directly explaining coverage
  (mode 12's grey blobs) intermittently dropping to zero at the same spot,
  and consequently the transmittance volume (mode 14) flickering too.
- **A1's removed entity-mask gating**: pass 8's simplified condition
  (`minimum_bin != 0xffffffffu` alone) was verified as a conservative
  widening, never a narrowing, so it should not drop coverage that existed
  before — worth re-checking against this specific case regardless, since
  it is the item most directly touching per-cell occupancy decisions, and a
  widening that flips per-frame near a boundary could itself be a source of
  instability even if each individual frame's result is "valid".
- **A3's per-pass uniform/setting restructure**: worth checking only if A1
  and A2 are cleared — A3 changed when/how often settings are read and
  shaders configured, which is a stretch for a per-frame content flicker
  but not yet ruled out.
- Possible non-AVBOIT-item cause: hair animation/wind sway combined with
  the volume's 1/8-cell resolution could always have been capable of this
  under the right conditions, and this session's other AVBOIT changes
  happened to change timing/ordering enough to newly expose it rather than
  directly cause it. Not ruled out.

## Status

Not investigated. User asked to log this as a TODO. No code changes made.
Revisit when the user is ready to pick it up.

## Repro

Avatar with hair, camera zoomed in on the top/crown of the head (pinned
down further by the mode-12 screenshots above; exact strand/area still not
pixel-precise). Compare a pre-session viewer build against current HEAD at
a fixed pose: flicker present only in the current build. Reproduces in
AVBOIT debug mode 0 (normal), mode 12 (banded weight — see screenshots),
and mode 14 (sampled front transmittance). Does NOT reproduce in mode 15
(chosen divider stays steady), which rules out A6.

## Suggested next steps when picked up

1. Pin down the exact camera/avatar pose and hair region reproducibly
   (the mode-12 screenshots narrow it to the crown; get an exact pose).
2. Bisect by reverting A2's shader changes first (leading candidate; see
   above) against a fixed HEAD, rebuilding, and re-checking mode 12 at the
   pinned pose for the grey no-coverage blobs. A1 next if A2 is cleared;
   A6 is already ruled out by mode 15's stability, no need to re-check it.
3. If A2 is the cause, look specifically at whether
   `gAVBOITCellDepthTarget`'s bake in `finishDirectOccupancy()` can vary
   frame to frame for the same static geometry (draw order within a cell,
   partial writes, stale state from a previous frame not fully cleared).
4. Once isolated, re-verify the offending item's own "identical output"
   claim against this specific case — the item's plan entry will need a
   correction if the claim doesn't hold here.
