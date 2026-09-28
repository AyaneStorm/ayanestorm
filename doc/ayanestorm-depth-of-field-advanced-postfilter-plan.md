# AyaneStorm Advanced DoF (mode 1): CoC-moments postfilter — Plan

Author: chanayane@firestorm
Date: 2026-09-28
Scope: roadmap item 5 of the mode-1 renderer
(`doc/rendering-improvements-backlog.md`, "Remaining AyaneStorm DoF
implementation roadmap"): "Store first and second CoC moments and apply a
radius-aware postfilter. It should close sparse sampling gaps while
preserving aperture boundaries and mixed near/far edges."

## Context

Mode 1 gathers every blurred pixel from a bounded number of aperture taps
(16/32/96). Large discs are undersampled.
- Each pixel uses a random aperture phase, so the undersampling shows as
  fine noise. The user reports that quality directly changes the residual
  lock pattern.
- Today the only cleanup is a fixed 3×3 tent in the resolve
  (`reconstructTransparent()` in `asDepthOfFieldResolveF.glsl`, and the far
  tent), blended in above 6 px of radius. It ignores:
  - how far apart the taps actually are, so it is too small for large
    discs;
  - where different blur radii meet, so it smears a sharp near/far
    boundary as readily as noise.

Goal: every gather also stores the first and second moments of the source
CoC radius it accumulated. A new postfilter pass then smooths each gather
result:
- **width** follows the local tap spacing, which grows with the radius;
- **mixed-radius edges** are protected by comparing radius moments: a
  bilateral on the mean radius, widened by the local variance;
- **aperture boundaries** stay crisp, because the width never exceeds the
  tap spacing and the filter also compares coverage.

Everything stays on the OpenGL 4.1 fragment/FBO path. The resolve gains no
sampler; it is at the 16-sampler limit.

## Findings used

- Gather outputs, all at blur resolution (`asdepthoffield.cpp` `render()`):
  - `sFarTarget` (1 attachment; rgb, alpha 1 or 0);
  - `sNearTarget` (2 premultiplied layers);
  - `sTransparentFarTarget`, `sTransparentNearTarget`, `sRiggedFarTarget`,
    `sRiggedNearTarget` (1 attachment each, premultiplied).
- The transparent pass 1 already declares outputs at locations 1–3 and
  writes zeros there. The near pass 1 writes zeros to locations 2–3.
- Sprites are added into `sFarTarget` and into near attachment 1 after
  their gathers (`draw_sprites`). The postfilter must run **before** the
  sprites, so the analytic aperture shapes stay crisp.
- Tap distributions (point taps):
  - far: `sqrt` radius, local spacing ≈ `R·sqrt(π/N)`;
  - near and transparent: uniform radius, spacing at distance d ≈
    `sqrt(2π·d·R/N)`, worst at d = R.
- `glBlitFramebuffer` between same-size RGBA16F targets is already used
  (`prepareTransparentDepthCapture`, `snapshotRiggedCoverage`), so filtering
  into a scratch target and blitting back needs no new pattern.

## Design

### A. Moments written by the gathers

For each accumulated layer, the weighted moments of the source blur radius
(in full-resolution pixels) over the same weights as the colour:
`m1 = Σw·r / Σw`, `m2 = Σw·r² / Σw`.
- **Far** (`asDepthOfFieldFarF.glsl`): new output location 1 `frag_moments`
  = (m1, m2, 0, 0). The centre sample counts with its radius. The
  completion branch writes the completion's radius.
- **Near** (`asDepthOfFieldNearF.glsl`, pass 1): location 2 = (back m1,
  back m2, front m1, front m2).
  - Pyramid taps: the texel's mean radius is not stored. Use the band's
    representative distance: sources in band k reach at least edge a_k, so
    r ≈ midpoint of [a_k, b_k]. The pyramid's front weight already
    approximates the layer split. This is documented as an approximation.
- **Transparent** (`asDepthOfFieldTransparentF.glsl`, pass 1): location 1 =
  (m1, m2, 0, 0); pyramid taps as in the near gather.
- Targets gain one RGBA16F attachment each (`addColorAttachment`):
  - `sFarTarget` → 2 attachments;
  - `sNearTarget` → 3 attachments;
  - the four transparent targets → 2 attachments each.
- `ensureResources()` checks `getNumTextures()` so existing allocations are
  rebuilt.

### B. Postfilter pass (new `asDepthOfFieldPostfilterF.glsl`)

One shader for every layer, run into a shared scratch target
(`sPostfilterTarget`, blur resolution, RGBA16F, 1 attachment). The result is
blitted back into attachment 0 (or 1 for the near front layer) of the source
target.

Per pixel, with `layer` = premultiplied colour, `(m1, m2)` = moments and
`a` = coverage:
1. `r̄ = m1`, `var = max(m2 − m1², 0)`. If `r̄ < 2 px` or `a ≈ 0` and no
   neighbour has coverage, output the input unchanged, so in-focus detail is
   never touched.
2. **Width** = the local tap spacing, converted to blur pixels:
   - far: `s = r̄·sqrt(π/N)`;
   - near and transparent: `s = sqrt(2π·r̄·R/N)`;
   - clamped to [0.75, r̄/3] blur px.
   So it grows with the disc and with lower quality, and never exceeds a
   third of the disc radius.
3. **12 taps**: a rotated Vogel disc of radius `s`, plus the centre. For
   each neighbour n, the weight is `g(d/s) · exp(−(m1_n − m1_c)² / (2(σ² +
   var_c + var_n)))` with `σ = max(1, 0.15·r̄)`. The radius term keeps
   different-blur regions (a mixed near/far edge) from averaging; the
   variance lets pixels that are already mixed smooth with each other.
4. **Aperture boundary**: for premultiplied layers, multiply by
   `exp(−(a_n − a_c)²/0.08)` when the alpha difference comes from the
   aperture edge. Across the rim, coverage jumps inside one tap spacing, so
   the rim stays sharp while noise inside a flat region (small alpha
   differences) is averaged.
5. Output: the normalized premultiplied average. The moments stay in their
   attachment; the resolve does not need them.

Uniforms: `plane_kind` (0 far sqrt distribution, 1 uniform distribution),
`sample_count`, `max_radius`, `screen_res`, `target_res`, `channel` (which
attachment is colour, and which moment pair for the near back or front).
Existing reserved sampler names are used (`diffuseRect` colour,
`noiseMap` moments).

### C. Pass order in `render()`

- far gather → **postfilter far** → far sprites;
- near gather → **postfilter near back, near front** → near sprites;
- each transparent gather → **postfilter it**, right after its gather.
  The pyramid target is reused as before, and the postfilter uses its own
  scratch target.
- Resolve: the fixed tents (`reconstructTransparent` and the far tent) are
  bypassed when the postfilter ran (uniform `postfiltered`). They remain the
  fallback when it is off.

### D. Settings, UI, debug

- `settings.xml`, inside the existing AS block: `ASDepthOfFieldPostfilter`
  (bool, default 1). Added to the reset list in
  `ASDepthOfField.ResetDefault`.
- Floater Advanced tab: the checkbox "Sampling noise filter", with a reset
  button.
- `ASDepthOfFieldDebug` 25 shows the radius spread `sqrt(var)/R` as grey,
  i.e. where edges are protected. The moments are not bound in the resolve,
  which has no free sampler. So in debug 25 the postfilter writes this view
  into the far layer and the near layers instead of filtering, and the
  resolve shows it through its existing views: debug 3 for far, debug 4 for
  near. The debug clamp becomes 0..25.

### E. Snapshots

Snapshots run the same mode-1 path at snapshot resolution. The width is in
blur pixels derived from the radius and N, so it scales with resolution
without special cases.

## Files

- `indra/newview/asdepthoffield.cpp`: attachments, the scratch target, the
  postfilter program registration and passes, the setting, the debug clamp.
- Shaders under `indra/newview/app_settings/shaders/class1/deferred/`:
  - modified: `asDepthOfFieldFarF.glsl`, `asDepthOfFieldNearF.glsl`,
    `asDepthOfFieldTransparentF.glsl` (moments), `asDepthOfFieldResolveF.glsl`
    (tent bypass);
  - new: `asDepthOfFieldPostfilterF.glsl`.
- `indra/newview/app_settings/settings.xml`: the tagged AS block.
- `indra/newview/skins/default/xui/en/floater_as_depth_of_field.xml`.
- `scripts/testing/dof_reference.py`: a postfilter mirror and tests.
- `doc/ayanestorm-depth-of-field-final-implementation-plan.md`: execution
  record. Copy this plan to
  `doc/ayanestorm-depth-of-field-advanced-postfilter-plan.md` after approval.

Only owned `as*` files are touched, plus the existing tagged block in
`settings.xml`. No `ll*`/`fs*` edits, no shader or cache revision bump, no
build (the user builds).

## Verification

- **CPU** (`dof_reference.py`, mirror of the filter weights):
  - uniform defocused field with per-pixel phase noise (point-tap simulation
    reused from `dof_near_gather_sim.py`): after the filter, residual noise
    is at least 2× lower at N = 16/32/96;
  - a near/far edge (radii 4 and 24 px): colour leak across the edge no
    worse than without the filter (within 1 %);
  - an isolated aperture disc: rim width (10–90 %) grows by no more than one
    tap spacing;
  - energy: a filtered uniform premultiplied layer keeps its mean within
    0.5 %.
- **Runtime** (user build, mode 1, Cinematic and Low):
  1. Large far blur and the hair lock: the grain is smoother than before;
     Low improves most.
  2. A foreground edge over a far background: no halo or smear.
  3. A defocused light shape: the polygon edges stay crisp (sprites are
     unaffected).
  4. Toggling the new setting shows the difference; FPS cost is noted.
