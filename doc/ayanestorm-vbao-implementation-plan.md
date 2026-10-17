# VBAO (Visibility Bitmask AO) implementation plan

Author: chanayane@firestorm

## Context

GTAO keeps one maximum horizon per slice side. With depth-only input, any
foreground surface within the radius acts as an infinitely thick wall. Visible
result: an avatar in front of another darkens the one behind. The
`ASGTAOThinOccluderCompensation` heuristic only shortens depth reach (acts like
a smaller radius everywhere).

VBAO (Therrien, Levesque, Gilet 2023, "Screen Space Indirect Lighting with
Visibility Bitmask") replaces the horizon pair with a 32-sector occlusion
bitmask per slice. Each sample is a slab of finite `thickness` and occludes only
the sectors between its front and back angles, so light passes behind thin
foreground objects. Same slices, steps and depth reads as GTAO; the only extra
per-sample cost is a few ALU and bit operations.

Goal: third AO technique `VBAO` (`RenderAOTechnique = 2`), reusing the whole
GTAO pipeline (depth mips, Hilbert noise, edges, denoiser, fragment/compute
backends, bent normals, off-screen sample rejection, all GTAO settings), plus
one thickness setting. GTAO stays unchanged for A/B comparison.

## Design

### Shader: `asGTAOCommonF.glsl` (shared by fragment + compute, scalar + bent)

New uniforms: `uniform int gtao_bitmask;` and `uniform float gtao_thickness;`.
Uniform branch per slice (coherent, no new program objects). If GTAO-mode main
pass timing regresses from register pressure, move to a `GTAO_BITMASK`
permutation instead.

Per slice, when `gtao_bitmask != 0`:

- Keep existing slice setup (`n`, `cos_normal`, `projected_length`, `view_vec`,
  step distribution, LOD, off-screen rejection, falloff weight).
- `uint occluded = 0u;`
- Per sample, side `s` (+1 for uv0, -1 for uv1), `delta` as today:
  - `front = dot(normalize(delta), view_vec)`
  - `back  = dot(normalize(delta - view_vec * gtao_thickness), view_vec)`
  - Angles: `gtao_fast_acos`, then relative to normal:
    `a = clamp(s * -angle - n + HALF_PI, 0, PI)` style mapping exactly as the
    paper, giving front/back in `[-PI/2, PI/2]` around the normal; order
    min/max per side.
  - **Cosine weighting via sin mapping** (our addition): map each angle to
    `u = (sin(theta) + 1) * 0.5` instead of `theta / PI + 0.5`. In a 2D slice
    `cos(theta) d(theta) = d(sin(theta))`, so equal bit sectors are equal
    cosine-weighted measure and `bitCount` directly gives the cosine-weighted
    visibility, matching GTAO's integral (unoccluded slice = 1).
  - Falloff: scale the occluded span by `weight` (span = `(u1 - u0) * weight`,
    anchored at the front edge). Continuous, no extra noise.
  - Bits: `start = uint(u0 * 32)`, `count = uint(ceil(span * 32))`,
    `mask = count > 0 ? (0xFFFFFFFFu >> (32 - count)) << start : 0u`
    (guard `count == 32` / `start + count > 32` by clamping count to
    `32 - start`), `occluded |= mask`.
- Slice visibility: `visibility += projected_length * (1 - bitCount(occluded) / 32.0)`.
- Bent normals (`GTAO_BENT_NORMALS`): derive effective horizons from the
  visible span: `findLSB(~occluded)` / `findMSB(~occluded)` (all-occluded:
  keep current bent normal untouched for that slice), convert back to angles
  via `asin(2u - 1)`, feed existing `t0/t1` bent-normal formula. Cheap; exact
  for a single visible gap, approximate for multiple gaps.
- `GTAO_TERM_SCALE`, small-radius fade term, `pow(power)`, `max(0.03)`,
  packing and edges unchanged.
- The thin-occluder heuristic is skipped in VBAO mode (thickness supersedes
  it): use plain `distance` for the falloff weight there.

GLSL 4.10 has `bitCount`, `findLSB`, `findMSB`; compute 4.30 too.

### C++: `asambientocclusion.h/.cpp`

- `enum Technique { LEGACY_SSAO = 0, GTAO = 1, VBAO = 2 };`
- `requestedTechnique()`: map setting 1 -> GTAO, 2 -> VBAO, else legacy.
- Helper `bool usesHorizonPipeline(Technique t) { return t != LEGACY_SSAO; }`;
  replace each `!= GTAO` / `== GTAO` check (allocateResources, render,
  bindResult, visualizeResult, bentNormalsEffective).
- `render()`: set `sEffectiveTechnique` to the requested technique on success
  (find where it is set to GTAO now).
- `applyMainUniforms()`: set `gtao_bitmask` (requested == VBAO) and
  `gtao_thickness` = `llclamp(ASVBAOThickness, 0.01, 5)`.
- LL_INFOS/WARNS text stays "GTAO" tag (same pipeline).

### Settings: `settings.xml`

- `RenderAOTechnique` comment: add `2` VBAO.
- New `ASVBAOThickness` F32, default `0.35` m (avatar-scale slab; tune at
  runtime), Persist 1.

### UI (tagged `AS:Chanayane` comments already wrap these blocks)

- `floater_phototools.xml`, `floater_advanced_phototools.xml`,
  `panel_preferences_ayanestorm.xml`: add `<combo_box.item label="VBAO"
  name="vbao" value="2"/>`; update prefs tooltip.
- `quickprefs.cpp:1133`: `== 1` -> `!= 0` so the GTAO panel shows for VBAO.
  Also show/hide the new thickness row by `== 2`.
- `floater_phototools.xml` `P_AO_GTAO`: add Thickness row (text, slider
  0.01-2.0, spinner, reset) at `top=220`; panel height 216 -> 240 (already
  taller than the 161 px legacy panel; last element of the tab).
- `floater_advanced_phototools.xml`: no room in the 92 px overlay; thickness
  available in full Phototools and Debug Settings only.

### Docs

- After approval: copy this plan to `doc/ayanestorm-vbao-implementation-plan.md`
  (Copy-Item, verbatim).
- Short pointer section in `doc/ayanestorm-gtao-implementation-plan.md`.
- No shader version / cache revision bump.

## Critical files

- `indra/newview/app_settings/shaders/class1/deferred/asGTAOCommonF.glsl`
- `indra/newview/asambientocclusion.h`, `indra/newview/asambientocclusion.cpp`
- `indra/newview/app_settings/settings.xml`
- `indra/newview/quickprefs.cpp` (tagged edit, minimal)
- `indra/newview/skins/default/xui/en/floater_phototools.xml`,
  `floater_advanced_phototools.xml`, `panel_preferences_ayanestorm.xml`

## Verification

1. glslang: scratch copies of common + `asGTAOMainF.glsl` (4.10, with
   `GTAO_SLICES/STEPS` defines, scalar and bent) and common + `asGTAOMainC.glsl`
   (4.30, scalar and bent). Zero errors.
2. User build, then runtime:
   - Technique VBAO, white debug: avatar in front of another no longer darkens
     the one behind; contact AO (arm/torso, feet/floor, wall corners) still
     present.
   - Compare GTAO vs VBAO at same radius/power: similar overall darkness on
     open geometry (sin-mapping keeps the integral comparable).
   - `ASVBAOThickness` live: low = less halo, more light leaking; high ->
     approaches GTAO.
   - Bent normals on/off; Fragment and Compute backends.
   - Performance: `GTAO main` GPU zone, GTAO vs VBAO, and GTAO before/after
     the change (uniform-branch register-pressure check).
   - `RenderBufferVisualization = 7` for raw visibility.

## Implementation status (2026-10-17)

Implemented per plan, with one change to the bit mapping:

- Sectors use GTAO's own slice measure `cos(theta - n) * |sin(theta)|`
  (the integrand of GTAO's arc formula), not plain cosine (`sin` mapping).
  Position `u = (S(theta) - S(n - pi/2)) / T`, with `S` the signed GTAO arc
  integral and `T = cos(n) + n * sin(n)`. Slice visibility is
  `projected_length * T * (1 - bitCount / 32)`. With infinite thickness this
  equals GTAO up to 1/32 quantization, so Radius/Power keep their calibration.
- Both span ends are rounded to sectors (not floor/ceil), so samples grazing
  the hemisphere edge on flat surfaces set no bit.
- Bent normals: horizons from `findLSB/findMSB(~bits)`, inverted to angles by
  8-step bisection of `u(theta)` (bent mode only; dead code in scalar mode).
- Thin-occluder heuristic disabled in VBAO mode.
- Thickness row in full Phototools is shown only for VBAO (`quickprefs.cpp`).

Validation: glslang passes for main fragment (4.10), main compute (4.30) and
denoise, scalar and bent-normal.

Runtime (2026-10-17): built and running. `ASVBAOThickness` tuned by eye; 0.6 m
is the preferred value (closest overall look to GTAO while still letting light
behind thin foreground occluders), now the default (was 0.35).
