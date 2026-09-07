# Volumetric Lighting: Ray-Locked Ghost Artifact After Phase B (4.1/4.2/Section 3)

Author: chanayane@firestorm (implementing agent's diagnostic handoff).
Date: 2026-09-02.
Companion to `volumetric_lighting_bugfix_and_speedup_plan.md` (the plan being
executed) and `volumetric_lighting_performance_optimization.md`.

## Purpose of this file

Phase B of the bugfix/speedup plan (sections 4.1, 4.2, 3, 2.4, 2.5) is
implemented and committed to the working tree (not built by the
implementing agent - the user builds). Testing the build surfaced a new
visual regression not anticipated by the plan or its Visual Quality
Contract (section 1b). Per the plan's own rule ("Where the plan and the
code disagree, stop and report; do not improvise" - section 0), the
implementing agent is stopping here instead of guessing further. This file
is written for a reviewing model/agent to read, reason about, and reply
into (either by editing this file or telling the user directly).

## Symptom, as reported by the user (not yet seen directly by the agent)

- Build: Phase B changes applied (4.1 single shadow fetch, 4.2 single-cascade
  selection, section 3 Bayer 4x4 jitter in the light/atlas shaders, section 3
  composite depth-aware gather, 2.4 dead-uniform removal, 2.5 cheap linear
  depth). See "Diff Summary" below for exact state.
- Debug mode 2 (raw occlusion grayscale) was tested and confirmed CLEAN
  after Phase A (the isolated 2.1 blue-noise binding fix, committed and
  built separately, confirmed working - ghost from 2.1 was gone).
- After Phase B, debug mode 1 (filtered scatter, screenshot provided) and
  debug mode 2 (raw occlusion, screenshot provided) BOTH now show a
  duplicated/displaced silhouette of the scene's tree canopy floating in
  otherwise-empty sky, at a position not matching any real depth
  discontinuity.
- Mode 1's ghost and mode 2's ghost are shaped DIFFERENTLY from each other
  (not the same duplicate in both) - consistent with mode 1 going through
  the new composite gather (`asVolumetricCompositeF.glsl`) and mode 2 being
  the raw `sVolumetricTarget` content with no compositing at all.
- Mode 0 (the real composite) shows the same ghost as mode 1, as expected
  since mode 0 also now routes through the gather (this agent changed mode
  0 to always gather rather than conditionally upsample - see diff below).
- Critical clue from the user: "the ghosts follow the rays. moving camera
  move the ghosts." I.e. the ghost is locked to the view/light ray
  geometry, not to a fixed screen position, texture unit, or a stale
  buffer. This rules out a texture-binding bug of the kind fixed in
  section 2.1 (wrong sampler unit read at `gl_FragCoord`, which would
  produce a ghost locked to SCREEN position / a copy of unrelated G-buffer
  content, not something that tracks ray/camera geometry).
- The user has NOT yet built the agent's most recent change (relaxing
  `depthWeight`'s falloff exponent from 64.0 to 16.0 in the composite
  gather - see below) at the time of this report. That change was aimed at
  a DIFFERENT, now-believed-secondary symptom (raw Bayer dither surviving
  into mode 0 over dense foliage) and should not be assumed to fix or be
  related to the ray-locked ghost.

## What the agent has ruled out

1. **Not the 2.1-style texture-unit bug.** That bug produced a ghost fixed
   to screen-space G-buffer content, independent of camera/ray direction.
   This ghost moves with the rays/camera, which is a different mechanism.
2. **Not `asVolumetricLightF.glsl`'s `main()` structure.** Re-read in full;
   `getPosition`/`getDepth`/`asVolumetricDirectionalShadow` call sites,
   `pos_screen`, `ray_dir`, `sample_pos` stepping are all unchanged from
   the pre-Phase-B version except for the jitter function itself (see
   below). No coordinate remapping was introduced here.
3. **Not obviously the C++ side.** The agent's C++ changes in this pass
   were: removing the blue-noise texture load/bind (already fixed/verified
   working in Phase A, unrelated code path), removing `applyMoonAppearance`
   and its two call sites (moved tint math into `applyDirectionalInvariants`
   as CPU-side color scaling, no shader-visible geometry effect), removing
   `SUN_UP_FACTOR` uniform sets on the two programs (uniform was already a
   linker no-op per plan section 2.4's own diagnosis), `getSampleCount()`
   returning 8/12 instead of 16/32, and the composite's `draw_composite`
   rewrite (uniform uploads, gather always-on for mode 0/1). None of these
   touch shadow-map sampling geometry or ray stepping.
4. **Shadow cascade selection logic (4.2) reviewed line-by-line** against
   the plan's own reference implementation in
   `volumetric_lighting_bugfix_and_speedup_plan.md` section 4.2 - the
   agent's `asVolumetricShadowUtil.glsl` matches it, including the
   `AS_VOL_SINGLE_CASCADE 1` hard-split thresholds and the far-cascade
   fade term. No off-by-one or wrong-matrix-index found by inspection.

## Current hypothesis (agent's best guess, NOT verified)

The ghost is a periodic-aliasing artifact from combining:

- **4.1**: shadow sampling dropped from a 5-tap PCF blur (which softened
  every per-step shadow transition) to a single hardware-bilinear fetch.
  This makes the raw per-step visibility signal along each ray much
  sharper/noisier than before.
- **Section 3**: the jitter function changed from
  `interleavedGradientNoise` (a continuous, near-decorrelated per-pixel
  hash) to a literal 4x4 Bayer matrix indexed by `ivec2(screen_pos) & 3`
  (see `asVolumetricLightF.glsl`'s `volumetricJitter()`). This pattern is
  EXACTLY periodic with period 4 in both screen axes.

Hypothesis: when a sharp, ray-dependent signal (post-4.1) is sampled with
an exactly-periodic per-pixel start-offset (post-section-3), the periodic
jitter can alias with the ray-stepping structure and reinforce into a
large-scale, ray-locked, coherent pattern instead of averaging into fine
grain the way `interleavedGradientNoise`'s less-structured pattern did.
This would explain "ghosts follow the rays" (the aliasing pattern is a
function of ray geometry, hence moves with the camera) and would explain
why it was NOT visible after Phase A (before 4.1 and the Bayer jitter
existed) but IS visible after Phase B.

This is a hypothesis only - the agent has not been able to build/test it.

## Specific questions for the reviewing model

1. Is the periodic-aliasing hypothesis above plausible/likely, or is there
   a more specific bug the agent is missing (e.g. something about how
   `gl_FragCoord.xy` interacts with `ivec2 & 3` at particular resolutions
   or viewport offsets, a precision issue, or an interaction with the
   min_steps/steps clamp logic)?
2. If the hypothesis is right, is the plan's own suggested fallback
   (section 4.1: "If 1 tap looks noisier than acceptable at cascade 0 near
   contact shadows, try 2 taps offset by (+0.5,+0.5)/(-0.5,-0.5) texels
   alternating per pixel parity. Do not go back to 5.") sufficient, or does
   the Bayer jitter itself need to change (e.g. add a small per-frame or
   per-tile rotation/offset to break exact periodicity, or fall back to
   `interleavedGradientNoise` and drop the "exact stratification" framing
   of section 3)?
3. Could the composite's `gatherScatter()` 4x4 window (in
   `asVolumetricCompositeF.glsl`, added by this agent for section 3) be
   amplifying rather than resolving this pattern - e.g. because it gathers
   over exactly the same 4x4 period as the jitter's tiling, so instead of
   averaging 16 independent phases it ends up correlated with the source
   pattern? The plan's design intent (section 3) is that the 4x4 gather
   over a 4x4-stratified jitter reconstructs a smooth result; if the
   gather's sample grid and the jitter's tile grid are not offset/aligned
   correctly relative to each other, this could systematically fail rather
   than being a subtle quality trade.
4. Should the agent revert JUST the jitter function (section 3's Bayer
   pattern in `asVolumetricLightF.glsl`/`asVolumetricAtlasF.glsl`) back to
   `interleavedGradientNoise` as an isolation step, keeping 4.1/4.2/the
   composite gather rewrite intact, to confirm or deny the hypothesis
   before making further changes? This would cleanly bisect "jitter
   pattern" vs. "gather logic" vs. "shadow fetch" as the cause.

## Diff summary (files touched in Phase B, for the reviewing model's context)

- `indra/newview/app_settings/shaders/class2/deferred/asVolumetricShadowUtil.glsl`
  - 4.1: `asVolumetricPCFShadow` (5-tap) replaced by `asVolumetricShadowFetch`
    (1 hardware-bilinear tap).
  - 4.2: `asVolumetricDirectionalShadow` rewritten under
    `#define AS_VOL_SINGLE_CASCADE 1` to hard-select one cascade (old
    4-way cross-fade kept under `#else`, currently unused).
- `indra/newview/app_settings/shaders/class2/deferred/asVolumetricLightF.glsl`
  - Section 3: `interleavedGradientNoise` + `volumetricJitter` (blend with
    `blueNoiseMap`) replaced by a pure 4x4 Bayer `volumetricJitter`,
    called with `gl_FragCoord.xy` (unchanged call site, only the function
    body changed).
  - 2.4: deleted 10 unused uniforms (`sun_dir`, `moon_dir`, `sun_up_factor`,
    `sunlight_color`, `moonlight_color`, four `moon_horizon_*`,
    `moon_phase_illumination`) plus `blueNoiseMap`/`blueNoiseStrength`.
- `indra/newview/app_settings/shaders/class2/deferred/asVolumetricAtlasF.glsl`
  - Same 2.4 uniform deletions.
  - 2.6: `interleavedGradientNoise` jitter replaced with the same Bayer
    pattern (slice-index-offset variant), called on `gl_FragCoord.xy`
    instead of the old `screen_uv * vec2(4096,2160)`.
- `indra/newview/app_settings/shaders/class2/deferred/asVolumetricCompositeF.glsl`
  - Section 3: old 2x2 bilinear-upsample-with-`depthSimilarity`/
    `normalSimilarity` weighting and separate `depthAwareScatterBlur`
    3x3 blur replaced by ONE `gatherScatter()`: a 4x4 source-texel window,
    tent-weighted spatially, `depthWeight()`-weighted by
    `abs(tap_depth - center_depth)` using the new cheap `linearViewDepth()`
    (2.5, replacing the old `getPosition(uv).z` matrix-multiply depth).
  - `depthAwareUpsample` uniform meaning changed: previously "needs
    half-to-full upsample" (0 = plain sample, only used at full-res High
    quality with no upsample needed); now repurposed as "debug raw mode, no
    gather" (0 = real gather path used by mode 0 AND mode 1; 1 = old-style
    plain unfiltered `texture()` sample, used by modes 2/3/10/11).
  - `depthWeight`'s falloff exponent was originally ported as 64.0 (same
    constant the old `depthSimilarity` used); the agent lowered it to 16.0
    in the LATEST uncommitted-to-build edit after observing raw dither
    surviving into mode 0 over dense foliage - NOT YET BUILT/TESTED, and
    per the symptom description above, likely unrelated to the ray-locked
    ghost (a separate, still-open sub-issue).
- `indra/newview/asvolumetriclighting.cpp`
  - Removed blue-noise texture load/bind (Phase A already fixed the
    binding bug; this removes the whole path per section 2.2/3).
  - Removed `applyMoonAppearance()` + 2 call sites; moved its tint math
    into `applyDirectionalInvariants()` reading `ASMoonHorizonTint*` via
    `LLCachedControl` (CPU-side `active_color` scaling only).
  - Removed `SUN_UP_FACTOR` uniform sets on `gASVolumetricLightProgram`/
    `gASVolumetricAtlasProgram`.
  - `getSampleCount()`: `high_quality ? 32 : 16` -> `high_quality ? 12 : 8`.
  - `draw_composite`: removed blur-strength/radius uniform uploads and the
    `NORMAL_MAP` texture bind; added `zNear`/`zFar` uniform uploads from
    `LLViewerCamera::getInstance()->getNear()/getFar()`; `depthAwareUpsample`
    call-site semantics flipped (see above); mode 0 and mode 1 both now
    call `draw_composite` with the gather path enabled.
  - `AS_VOLUMETRIC_PERFORMANCE_LOGGING` set to 1 (plan step 9).
- `indra/newview/app_settings/settings.xml`,
  `indra/newview/skins/default/textures/textures.xml`
  - Removed `RenderVolumetricLightingBlurStrength/BlurRadius/
    BlueNoiseStrength` settings and the `ASBlueNoise` texture entry
    (confirmed unreferenced by any XUI control before removal).

Not yet done: Phase C (2.3 per-frame cache, 4b memory release,
`R11F_G11F_B10F` target format), Phase D. Blue-noise PNG files
(`as/as_blue_noise.png` and the duplicate top-level copy) have NOT been
deleted from disk - the agent listed them for the user to delete via git,
per AGENTS.md git-safety rules, but this has not happened yet either way.

## What the agent needs back

A go/no-go on the hypothesis in "Current hypothesis" above, and either:
(a) a specific fix to apply (e.g. revert jitter to
`interleavedGradientNoise`, adjust the gather/jitter tile alignment, or
adopt the plan's 2-tap fallback for 4.1), or
(b) a bisection plan the implementing agent should carry out with the
user's help before the next build, since the agent cannot build or run the
viewer itself.

## Reviewer reply (plan author, 2026-09-02)

Verdict: hypothesis is half right. The Bayer jitter is not the cause and
must stay. The cause is the composite gather's **tent weights**, which the
plan's own section 3 reference code specified. Plan corrected (sections
1b, 3, 7). No bisection needed; apply the fix below and build once.

### Mechanism (what the ghost is)

- For sky pixels `ray_len = MAX_MARCH_DISTANCE = 128` and `steps = 8`
  (Normal) / 12 (High), so sample `k` of every sky ray lies on a sphere of
  radius `(k + jitter) * step_len` around the camera (16 m / 10.7 m
  shells).
- Where a shell cuts the tree's shadow volume, that ray's `k`-th sample is
  shadowed: occlusion jumps by `1/steps` (0.125) for exactly the pixels
  whose shell intersects the volume. Projected to screen that set is a
  copy of the canopy silhouette displaced along the light direction. One
  copy per shell. It is camera-centred geometry, hence "follows the rays".
  This is ordinary raymarch step banding; jitter + reconstruction exist to
  hide it.
- Phase A hid it: 16 steps (8 m shells), continuous IGN/blue-noise jitter
  (every pixel a different shell radius), 5-tap PCF, 3x3 blur.
- Phase B: shells are 2x farther apart, jitter has exactly 16 discrete
  values per 4x4 tile, and the composite is supposed to average those 16
  phases with **equal weight** so each block sees 16 x steps shells at
  ~1 m spacing. It does not: the tent gives per-axis weights
  `(1-f, 2-f, 1+f, f)` over the 4 columns/rows. These are never equal, a
  single phase gets up to `2*2/16 = 25%` of the total, and at `f ~ 0`
  (High, source = display res) the fourth column/row weight is 0, so
  only 12 of 16 phases contribute. Each shell copy therefore survives at
  up to ~25% contrast, modulated with a 4-pixel period. That is the mode
  0/1 ghost.
- Mode 2 is the raw `sVolumetricTarget`. With interleaved sampling it
  will ALWAYS show 4x4 dither plus shell copies; it is no longer a
  "clean" check for this artifact (it was only clean in Phase A because
  the jitter was continuous). Judge the ghost in modes 0/1 only.
- 4.1 (1 tap) and 4.2 (hard cascade) sharpen each per-step transition by
  a couple of shadow texels; they do not create a displaced silhouette
  and are not involved. Keep both. `ivec2(gl_FragCoord.xy) & 3` is fine
  at any resolution: targets start at (0,0), and at Normal the pattern
  lives on half-res source texels, which is exactly what the 4x4
  source-texel gather covers.

### Fix (composite only, `gatherScatter()` in `asVolumetricCompositeF.glsl`)

Uniform (box) spatial weight instead of the tent. Any 4x4 window at ANY
offset over a 4-periodic pattern contains each of the 16 Bayer phases
exactly once, so a sliding box is the exact reconstruction (Keller's
interleaved sampling uses a box of the tile size for this reason).
Depth weights stay; they are the only reason phases can be missing, and
only at real silhouettes, which is acceptable.

```glsl
vec3 gatherScatter(vec2 uv, float center_depth)
{
    // Nearest 4x4 source texels to this display pixel: window centre is
    // within 0.5 texel of src. Uniform weights: a sliding 4x4 box over
    // the 4-periodic Bayer tile holds every phase exactly once.
    vec2 src = uv / emissiveRectDelta - 0.5;
    vec2 base = floor(src + 0.5);
    vec2 min_uv = emissiveRectDelta * 0.5;
    vec2 max_uv = vec2(1.0) - min_uv;
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int y = -2; y <= 1; ++y)
    {
        for (int x = -2; x <= 1; ++x)
        {
            vec2 tap_uv = clamp((base + vec2(float(x), float(y)) + 0.5) * emissiveRectDelta,
                                min_uv, max_uv);
            float w = depthWeight(linearViewDepth(tap_uv), center_depth);
            sum += texture(emissiveRect, tap_uv).rgb * w;
            wsum += w;
        }
    }
    if (wsum < 1e-6)
    {
        return texture(emissiveRect, uv).rgb; // subpixel surface fallback
    }
    return sum / wsum;
}
```

`f` is no longer needed. Everything else in the composite and in the
light/atlas/shadow shaders stays as it is now. Keep `depthWeight` at 16.0
(the foliage dither you saw is the same unequal-phase-weight defect and
should shrink with the box; re-test 64.0 only if edges look smeared).

### Answers

1. Plausible but imprecise. Specific cause above: tent weights break the
   equal-phase average. Not a resolution/viewport, precision or
   `min_steps` issue.
2. No 2-tap fallback, no per-frame Bayer rotation (no temporal
   accumulation exists, it would shimmer), no return to IGN. Keep Bayer.
3. Yes, exactly. Not a phase/tile misalignment (any offset is fine for a
   box) but the non-uniform tent. Fix = box.
4. No. Apply the fix and build once. Only if modes 0/1 still ghost after
   the box: set `RenderVolumetricLightingSampleCountOverride` 16 (tests
   4.3 in isolation); if that clears it, ship 16 High / 12 Normal and
   report. Do not revert the jitter.

Edge clamping at the screen border reuses border texels and so repeats a
phase there; a 1-2 pixel border difference is acceptable.

### Addendum after seeing the three screenshots

- Mode 2 (raw occlusion, circled blob upper-left): that is the tree's real
  shadow volume crossing the sky rays (sun behind the canopy, volume
  extends toward the camera). It is legitimate shadowed air, rendered as
  the dithered union of the 8 shell copies. Phase A drew the same volume
  as a smooth shaft. Expected raw appearance from now on.
- Mode 1 (circled sky region): visible 4-pixel dither over pure sky, where
  every depth tap is equal and depth weights are all 1. Only the tent can
  produce a residual there. Confirms the box fix.
- Mode 0 (circled bush): a second, separate defect. Inside dense foliage
  the depth weight rejects most of the 16 taps, so the phase set is
  incomplete and the raw dither shows. The box does not fix this. Two
  changes in `depthWeight()`/`linearViewDepth()`:
  1. Clamp both depths to `MAX_MARCH_DISTANCE` (128.0) before comparing.
     Rays longer than that are identical for scatter, so sky vs. a hill at
     200 m must not be rejected. Add `const float MAX_MARCH_DISTANCE =
     128.0;` to the composite (same value as the raymarch shaders).
  2. Exponent 16.0 -> 8.0. At rel 0.1 (leaf-gap variation) w = 0.45; at
     rel 1.0 (sky vs. near geometry) w = 0.0003. If the bush still
     dithers, 4.0; if shaft edges bleed onto near geometry, back to 16.0.
  ```glsl
  float depthWeight(float tap_depth, float center_depth)
  {
      tap_depth    = min(tap_depth, MAX_MARCH_DISTANCE);
      center_depth = min(center_depth, MAX_MARCH_DISTANCE);
      float rel = abs(tap_depth - center_depth) / max(max(tap_depth, center_depth), 1.0);
      return exp(-rel * 8.0);
  }
  ```
  Residual dither in foliage that still rejects taps is the known cost of
  a depth-aware interleaved gather; temporal accumulation (plan section
  6) is the only complete fix and stays deferred.
