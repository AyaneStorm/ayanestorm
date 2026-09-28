# AyaneStorm Advanced DoF (mode 1): highlight sprites and foreground layers — Plan

Author: chanayane@firestorm
Date: 2026-09-28
Scope: roadmap items 2 and 3 of the mode-1 renderer
(`doc/rendering-improvements-backlog.md`, "Remaining AyaneStorm DoF implementation roadmap").

## Context

Mode 1 (Advanced, live, screen-space) stopped at its first design when work
moved to the aperture-sampled mode 2. Two visible defects remain:

- **Bokeh on small lights** is built from a bounded number of gather taps (16/32/96).
  Isolated lights come out as dotted, phase-noisy discs, and blade count,
  roundness and rotation are barely visible.
- **Foreground silhouettes.** The near layer is one premultiplied layer for every
  foreground depth. The background plate behind a defocused foreground is the
  average of all non-foreground taps in the disc (`asDepthOfFieldFarF.glsl`,
  fill branch), so in-focus mid-ground colour leaks into it.

Goal: coherent, energy-conserving aperture-shaped highlights (item 2), plus
two ordered foreground layers with a depth-biased background completion that
is blurred before compositing (item 3). Everything stays on the OpenGL 4.1
fragment/FBO path. There is no compute path.

## Findings used by this plan

- `ASDepthOfField::render()` (`indra/newview/asdepthoffield.cpp:603`) receives
  linear HDR before exposure (`pipeline.cpp:9231-9251`), so thresholds
  must be relative, not absolute.
- **Orientation mismatch with mode 2 (verified by algebra on
  `ASDoFCamera::lensProjection` shear + `lensModelview`).**
  - In mode 2 the image shift is `P00·dx·(1/f − 1/d)`, so the far PSF is +aperture and the near PSF is −aperture.
  - Mode 1 has these reversed:
    - The far gather samples `uv + disk` (PSF −shape).
    - The near gather samples `uv − disk` (PSF +shape).
    - Both transparent planes sample `uv − disk`.
  - Polygon phase also differs. Mode 1 puts an edge centre at `rotation` (`mod(angle − rot + ½sector, sector) − ½sector`). Mode 2 puts a vertex there (`fmod(angle, 2h) − h`, `asdofaperture.cpp:112`).
  - All of this is visible only with odd blade counts. The sprites need one convention, so the gathers are aligned to mode 2.
- An instanced, attribute-free draw already has a precedent: `asAVBOITEarlyDepthV.glsl`, using
  `gl_VertexID`/`gl_InstanceID`, `mFeatures.attachNothing = true`, and
  `gPipeline.mScreenTriangleVB->setBuffer()` before the draw (`asavboit.cpp:2039-2050`).
  `glDrawArraysInstanced` is loaded (`llgl.cpp:1906`).
- The manual mip pattern is `TMG_MANUAL` + `generateMips()` (`asdofrenderer.cpp:647-665`).
- The aperture area CDF `bladeCdf()` is in the anonymous namespace of `asdofaperture.cpp:125`.
- **Texture-unit budget.** The GL 4.1 minimum is 16 fragment units. The resolve already
  uses 14, so sprites must not add resolve samplers.

## Design

### A. Shape convention alignment (prerequisite, small)

- In `asDepthOfFieldFarF.glsl`, `NearF.glsl` and `TransparentF.glsl`,
  `apertureSample()` puts a vertex at `rotation`:
  `local = mod(angle − rotation, sector) − ½sector`, the same as mode 2.
- Gathers sample so that far PSF = +shape and near PSF = −shape:
  - far: `uv − disk·r`;
  - near: `uv + disk·r`;
  - transparent: sign by `plane`.
  - The radius acceptance math is unchanged.

### B. Item 2 — aperture-shaped highlight sprites (opaque highlights)

The pipeline has four new steps, all in owned code:

1. **Cell reduce** (new `asDepthOfFieldHighlightF.glsl`, `highlight_pass = 0`).
   - Target: a cell grid of 8×8 full-resolution pixels with two RGBA16F attachments, `TMG_MANUAL`.
   - For each pixel p in the cell, `detect(p)` reads the original opaque colour and the opaque CoC:
     - **isolation** = smoothstep(k, 2k, L / max luminance of an 8-tap ring at 6 px). k is `ASDepthOfFieldHighlightIsolation`, default 2.
     - **defocus gate** = smoothstep(2, 4, CoC radius in px). The radius is signed CoC × the near or far radius.
     - **excess** = isolation · gate · max(c − ring mean colour, 0).
   - Bright lines and large bright areas fail the ring test and stay in the gather. In-focus pixels (radius < 2 px) are never touched, and that is also where the resolve uses the original colour.
   - Output att0: (Σ excess.rgb, occupied). Output att1: (luminance-weighted centroid uv, weighted signed CoC, Σ luminance).
   - Then `generateMips` on att0: the top level's `.a` × cell count gives the approximate occupied-cell count.
2. **Gather input** (same shader, `highlight_pass = 1`).
   - Writes a full-resolution RGBA16F `sGatherInputTarget` = opaque colour − excess, but only for cells kept under the budget.
   - Keep rule: `count ≤ budget || hash(cell) < budget / count`, with the budget from `ASDepthOfFieldHighlightMaxSprites` (default 4096). Over budget, a dropped cell's highlight stays in the gather, so energy is never lost.
   - The far gather, the near gather and the background push read this target instead of `opaque_color`. The resolve and the transparent gathers keep the originals, so the residual `source − opaque·(1−a)` is unchanged.
3. **Sprites** (new `asDepthOfFieldSpriteV.glsl` / `SpriteF.glsl`, `attachNothing`).
   - One `glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2·cells)` per plane: two triangles per cell, so the screen-triangle VB's 3 vertices are never exceeded.
   - The vertex shader `texelFetch`es the cell. An empty, dropped or other-plane cell becomes a degenerate triangle. Otherwise it emits a quad of half-size R·max(anamorphic, 1) + 1.5 px at the centroid.
   - The fragment shader evaluates the analytic aperture:
     - It undoes anamorphic and rotation; the near plane is point-reflected.
     - r/R is compared with the mode-2 `boundary(θ)`, with 1 px antialiasing.
     - It writes `E · aa / (unitArea · R²)`, radiance per full-resolution pixel, with alpha 0.
   - Blending is additive ONE/ONE:
     - far sprites go into `sFarTarget` after the far gather;
     - near sprites go into the near front-layer attachment after the near gather.
   - So the resolve gains no sampler. Far sprites are automatically hidden where `far_blend = 0` (in-focus content in front), and they appear in the plate behind defocused foreground.
   - `unitArea`: a new public `ASDoFAperture::unitArea(const Shape&)` (anamorphic × blades × 2 × `bladeCdf(half)`, or π for a circle), passed as a uniform.
4. **Legacy highlight boost** (`highlightWeight`) is unchanged. It still weights the gathers' residual.

Known limitation (documented, not solved here): highlights that exist only in transparent layers (alpha lamp bulbs, glow jewellery) are not extracted. They stay in the transparent gathers. Opaque lights seen through glass are handled correctly, because the sprite is added before the transparent layers composite over.

### C. Item 3 — ordered foreground layers and background completion

1. **Two near layers.**
   - `sNearTarget` gets a second attachment (`addColorAttachment`). The near shader writes a back layer and a front layer by source radius, with a soft split at `0.5 · near_max_radius` (smoothstep 0.8–1.2×).
   - Each layer keeps its own premultiplied colour, its coverage normalization, and the existing centre-ownership rule for the layer the centre pixel falls in.
   - Resolve: back over colour, then front over colour. Both use the existing `reconstructTransparent()` tent.
   - Resolve samplers: 14 → 15.
2. **Background push** (new `asDepthOfFieldBackgroundF.glsl`, `bg_pass = 0`).
   - Target: blur resolution, two RGBA16F attachments, `TMG_MANUAL`.
   - Each blur pixel fetches the 2×2 full-resolution texels of its footprint from the gather input and the opaque CoC (point fetch, no sign mixing). Only non-foreground texels count (CoC ≥ −ε).
   - Weight w = exp2(3·max(coc, 0)), so farther surfaces dominate over in-focus mid-ground.
   - att0 = (rgb·w, w); att1 = (coc·w, valid fraction). Then mips are generated on both attachments.
3. **Background pull** (`bg_pass = 1`), into blur-resolution RGBA16F `sBgCompleteTarget` (rgb, coc).
   - Valid pixels copy themselves.
   - Hole pixels (foreground) take the finest mip level whose valid fraction is ≥ 0.1, blended with the next coarser level. Levels are capped at `log2(near radius at blur res) + 2`.
   - If nothing is found, the pixel falls back to the gather input with CoC 0.
4. **Far gather uses the completion.**
   - At foreground-centre pixels, the old fill branch is replaced: the pixel runs the normal far gather over `sBgCompleteTarget` (colour and CoC), or outputs it unblurred when that CoC is under 0.5 px.
   - At far pixels, taps that land on foreground read the completion instead of being rejected, so the background behind silhouettes contributes.
   - The resolve's existing `background_fill` path then uses a depth-biased, blurred plate. `ASDepthOfFieldNearRadius` drives the pull level cap; the `foreground_radius` uniform is removed.

### D. Settings, UI, diagnostics

- **`settings.xml`**, inside the existing AS block:
  - `ASDepthOfFieldHighlightSprites` (bool, default 1);
  - `ASDepthOfFieldHighlightIsolation` (F32, 2.0, range 1.2–8);
  - `ASDepthOfFieldHighlightMaxSprites` (S32, 4096, range 256–32768).
  - The `ASDepthOfFieldDebug` comment is extended with 16 extracted highlight energy, 17 near back layer, 18 near front layer, 19 background completion.
- **Floater** `floater_as_depth_of_field.xml`, Advanced tab: a checkbox and two sliders, with `enabled_control="ASDepthOfFieldUIAdvanced"` and reset buttons. The floater height grows only if needed.
- **`ASDepthOfField.ResetDefault`**: the three new names go into the list after the first three entries, which are the mode choices.
- **`releaseGatherResources()` / `ensureResources()`**: the new targets are allocated only in mode 1, and sprite targets only when sprites are on. On allocation or shader failure, `render()` returns false, the same transactional fallback as today.
- **Logging**: log once when active and once on failure.

### E. Out of scope

- Occupancy leftovers.
- Transparent-highlight extraction.
- Tile classification and the compute backend (item 4).
- The moments postfilter (item 5).

## Files

- `indra/newview/asdepthoffield.cpp`: passes, targets, uniforms, shader registration and the reset list.
- `indra/newview/asdofaperture.{h,cpp}`: `unitArea()`.
- Shaders under `indra/newview/app_settings/shaders/class1/deferred/`:
  - modified: `asDepthOfFieldFarF.glsl`, `asDepthOfFieldNearF.glsl`, `asDepthOfFieldTransparentF.glsl` (orientation only), `asDepthOfFieldResolveF.glsl`;
  - new: `asDepthOfFieldHighlightF.glsl`, `asDepthOfFieldSpriteV.glsl`, `asDepthOfFieldSpriteF.glsl`, `asDepthOfFieldBackgroundF.glsl`.
- `indra/newview/app_settings/settings.xml`: the tagged AS block.
- `indra/newview/skins/default/xui/en/floater_as_depth_of_field.xml`, which is owned.
- `scripts/testing/dof_reference.py`: new tests.
- `doc/ayanestorm-depth-of-field-final-implementation-plan.md`: execution record. Also copy this plan to `doc/` after approval.

Only owned `as*` files are touched, plus `settings.xml` inside the existing tagged block. No `ll*`/`fs*` edits, no shader-version bump, no build.

## Verification

- **CPU tests** in `dof_reference.py`:
  - `unitArea` matches the sampled polygon area (circle, hexagon, rounded pentagon, anamorphic).
  - Extraction plus splat conserves energy within 1% on a synthetic defocused point, a light straddling cells and the over-budget case.
  - The orientation convention matches `viewer_*` mode 2 for a 3-blade aperture.
- **Runtime** (user build): Exact OIT and Standard, night scene with small lights.
  1. A triangle aperture in mode 1 and in mode 2 gives the same bokeh orientation, far and near.
  2. Isolated lights show clean, uniform polygon discs with visible blades, rotation and anamorphic squeeze. Exposure does not change when sprites are toggled.
  3. Neon strips and bright windows still blur through the gather (no sprites). Debug 16 shows only point lights.
  4. Hand or hair in front of focused mid-ground over a far background: no mid-ground colour leaks into the blurred foreground edge (debug 19). A foreground in front of a nearer foreground is ordered correctly (debug 17/18).
  5. FPS: DoF off vs mode 1 before and after, at quality 0/2.
