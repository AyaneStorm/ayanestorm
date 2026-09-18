# Rendering Improvements Backlog

Candidate rendering features/fixes for AyaneStorm, ranked most to least
important. Findings only, except item 0 which is unfinished in-tree work
being resumed rather than a fresh proposal.

## 0. Froxel-based volumetric fog (branch `volumetric-fog`) — promising, but never got to a working state

An unmerged `volumetric-fog` branch (last commit 2026-08-25, ~3,350 lines
across a new `asvolumetricfog.{h,cpp}` module, six new shaders, and
forward-consumer conversions in alpha/PBR-alpha/material/water/fullbright)
attempted to replace the current volumetric lighting system with a true
froxel (3D grid) approach: exponential height density, two-octave advected
noise, Henyey-Greenstein phase, a logarithmic-Z associative prefix-scan for
light transport, temporal reprojection, and per-consumer depth-aware
integration so forward-rendered alpha/water surfaces would sample the fog
consistently with the deferred composite. Architecturally more capable than
`asvolumetriclighting`'s screen-space raymarch, in theory.

**In practice, it never worked well.** The implementation log
(`doc/ayanestorm--volumetric-fog-replacement-implementation.md` on that
branch) shows individual pieces confirmed live via runtime readback
(allocation, injection, transport, temporal reconstruction, opaque composite),
but the visual result stayed wrong throughout: a persistent directional
shadow leak (sunlit fog visibility bleeding through opaque walls as
tree-shaped silhouettes) went through several rounds of root-causing and
fixes — wrong shadow cascade selection, then a temporal-history disocclusion
gap, then a forward-consumer depth-interpolation issue — without ever
converging on a clean, confirmed-correct result. Each fix uncovered a new
leak rather than closing the issue out. The branch stops with the five
forward-consumer helpers left in a deliberately degraded, conservative state
(floor-boundary-only lookup, no partial-segment interpolation) as a
correctness safety net, not as a finished feature.

**Why listed here at all:** worth knowing about before starting anything
fog/lighting-adjacent, since it represents real prior effort and a documented
trail of what didn't work — useful context to avoid repeating the same dead
ends, whether that means eventually debugging this branch further, restarting
with a different technique, or deciding the added complexity of a full froxel
system isn't worth it relative to the existing raymarch. Not treated as a
priority item to simply "finish" — its track record so far suggests the
remaining problems may be more fundamental than the last debugging session
assumed.

## 1. Glow/bloom pipeline: dormant luminance threshold + resolution ceiling (bug) — DONE (bok)

**What's wrong (verified directly against source):**
- `pipeline.cpp:8164` hardcodes `gGlowExtractProgram.uniform1f(LLShaderMgr::GLOW_MIN_LUMINANCE, 9999)`.
  The extraction shader thresholds via `smoothstep(minLum, minLum+1.0, luminance)`,
  so a value of `9999` makes the threshold permanently unreachable — luminance-gated
  bloom is silently inert for every scene, always.
- `RenderGlowMinLuminance` exists as a live `settings.xml` key (`:12765`) and a
  `pipeline.h:1134` static member, but is **never read** anywhere in `pipeline.cpp`.
  A user-facing setting that does nothing.
- `pipeline.cpp:1519,1524` clamps the glow buffer to `llmin(512, ...)` and
  allocates a non-square `512 × glow_res` target. The *iteration* pass at
  `:8208` already supports resolutions up to 1024, so the 512 allocation clamp
  is the actual bottleneck — `RenderGlowResolutionPow` cannot exceed what the
  buffer allocation allows regardless of the setting's own nominal range.
- Confirmed pre-existing in stock upstream Firestorm (checked against
  `.phoenix-firestorm-master`): identical `9999` hardcode at the equivalent
  line, identical dead `RenderGlowMinLuminance` member/setting, identical
  `llmin(512, ...)` buffer clamp despite the same 1024-capable iteration pass.
  Not introduced by AyaneStorm — inherited upstream, but live in the tree
  right now and worth fixing here regardless of origin.

**Why ranked #1:** the only item here that is an active correctness defect,
not a missing/weaker feature. Low risk, self-contained fix inside
`generateGlow()`/the screen-buffer allocation path; the uniform and GLSL
threshold logic already exist and are wired, just fed a dead constant and an
undersized buffer.

**Fix applied:** `RenderGlowMinLuminance` wired up as a real cached setting
(static definition, `connectRefreshCachedSettingsSafe`, `refreshCachedSettings()`
read) and the hardcoded `9999` at the extraction uniform call replaced with the
live value. Glow buffer allocation clamp raised from 512 to 1024 to match the
iteration pass's existing ceiling. All changes in `pipeline.cpp` wrapped in
`<AS:Chanayane>` ownership tags with original code preserved as comments.

**Caveat found after the initial fix:** the `9999` hardcode traces to LL's
"Move glow extract to be after tonemapping" (SL-19513), which moved extraction
to sample the post-tonemap frame (`mPostMap`) without re-deriving the
threshold for the new color space. The stock `RenderGlowMinLuminance` default
of `1.0` was calibrated for pre-tonemap linear HDR luminance (which can exceed
1.0); against post-tonemap luminance (compressed to roughly 0.0-1.0), a `1.0`
default is still effectively unreachable — un-hardcoding the value alone would
have "enabled" a still-inert feature. Checked how three other LL-derived
forks in-tree handle this: Black Dragon defaults to `0.0` (no gate), Aperture
Viewer defaults to `0.8` with an explicit post-tonemap comment, mayatonton's
AyaStorm kept `1.0`. Adopted Aperture's approach: `settings.xml` default
lowered to `0.8` with an updated comment noting the value is now
post-tonemapping (typically 0.7-0.9), tagged `<AS:Chanayane>`. Not yet
built/tested at runtime.

## 2. Motion blur (screen-space, velocity-buffer-driven)

AyaneStorm has no velocity buffer and no motion blur code at all. Add an
AyaneStorm-original implementation: a velocity render target derived from
current-vs-previous view/projection matrices, sampled by a per-pixel blur
along the local velocity vector, with a user-tunable strength/max blur length.

**Approach:** new `as`-prefixed offload module (own `.h`/`.cpp`, own shader
pair, own settings), following the `ASExactOIT`/`ASVolumetricLighting`
precedent — `pipeline.cpp` should only need small tagged call-outs, not
inline logic.

**Why ranked #2:** a fully-absent, substantial, high-visual-impact rendering
technique. Higher complexity than #1 (new render target, new shader pair, new
pipeline hook) but well-precedented by AyaneStorm's own existing offload
modules.

## 3. High-quality depth of field with depth-gated near-blur discrimination

AyaneStorm's DoF path only has a single-scale circle-of-confusion (CoF) blur
with no discrimination against background bokeh bleeding onto in-focus
foreground edges. Add an alternative high-quality DoF mode: a larger CoF
radius (e.g. ~4x) combined with a depth-gated sample-acceptance test so
out-of-focus background samples are rejected near in-focus foreground
silhouettes, producing materially cleaner bokeh than the stock single-scale
blur.

**Why ranked #3:** a real, visible image-quality upgrade to an existing,
already-shipped feature (DoF) rather than a new pipeline stage — moderate
effort (one alternate fragment shader, one quality toggle) for meaningfully
better photographic output, which matters directly for AyaneStorm's
visual-realism goals.

## 4. DoF-coupled chromatic aberration

AyaneStorm already ships a radial chromatic aberration pass
(`aschromaticaberration.cpp/h`, `deferred/aschromaticaberrationF.glsl`)
with strength/falloff/center controls in Camera Effects, but its per-channel
(R/B) offset radiates from a fixed artistic center point and is not linked to
depth of field — it approximates lateral (optical-center) chromatic
aberration only. Longitudinal chromatic aberration, where fringing strength
tracks CoF radius so heavily out-of-focus regions pick up extra color
fringing, is not implemented. Add this as a DoF-coupled variant/mode of the
existing effect rather than a new one.

**Why ranked #4:** small, self-contained shader addition layered on top of
the DoF pass; real value for photographic realism but cosmetic/optional,
ranked below the DoF quality fix itself since it depends on nothing else
being broken today.

## 5. Concentrated on-axis "sun beam" pass as a supplement to volumetric lighting

AyaneStorm's `asvolumetriclighting` raymarch is a strong, physically-elaborate
general volumetric/godray system (multi-cascade shadows, transmittance,
local lights, transparency atlas). It does not currently have a second,
separate, tightly-concentrated on-axis sun-beam effect — a distinct additive
pass tuned for a narrow, high-contrast shaft directly around the sun disc
(effectively a very tight forward-scattering phase, much narrower than the
general atmospheric raymarch would naturally produce) — layered on top of the
existing volumetric result for shots directly facing the sun/moon.

**Why ranked #5:** would be a genuine visual addition, not a duplicate of the
existing system, but it is a supplementary flourish on an already-strong
feature rather than filling a gap — lower priority than fixing/adding
capabilities that are currently fully absent (motion blur, HBAO) or broken
(glow threshold).

## 6. HBAO (horizon-based ambient occlusion)

AyaneStorm only has standard SSAO. Add an AyaneStorm-original HBAO option: a
multi-direction horizon raymarch for more directionally-accurate contact
occlusion than SSAO provides, selectable as an alternative AO mode.

**Why ranked #6:** substantial and visually meaningful, but lower urgency than
motion blur since SSAO already exists and is functional — this is a quality
upgrade/option rather than filling a total absence.

## 7. SSAO sample count is hardcoded (existing SSAO is untunable)

`aoUtil.glsl:89` hardcodes `for (int i = 0; i < 8; i++)` for the SSAO sample
loop. Expose this as a live setting wired through a dedicated uniform into the
GLSL loop bound, so users can trade AO quality for performance without a
rebuild.

**Why ranked #7:** small, low-risk, high value-for-effort — a single new
uniform plumbed through the existing SSAO shader, no new algorithm. Distinct
from item #6 (HBAO): this is about tunability of the *current* SSAO, not a new
AO technique. Natural quick win, possibly worth doing alongside #6 since it
touches the same shader file.

## 8. Physically-derived time-of-day color temperature shift

AyaneStorm's color grading (`ascolorgrading`) is a powerful general
image-space pipeline (OKLab grading, 24-band mixer, split toning, LUTs), but
it operates on the already-rendered frame. It does not currently include a
physically-motivated pre-render color-temperature shift: deriving a Kelvin
value from sun elevation (cooler near zenith, warmer near the horizon) and
multiplying it into the actual sun/ambient/cloud lighting terms before the
scene is lit, rather than grading the result afterward. This is a different
mechanism from image-space grading — it changes the actual light color the
scene is lit with, so it interacts correctly with all downstream lighting
(shadows, specular, etc.) rather than only tinting the final pixels.

**Why ranked #8:** a genuinely distinct technique worth having alongside the
existing grading system, but it's a subtle, cumulative-over-time enhancement
rather than a clearly-missing capability — lower priority than fixing broken
things or adding wholly-absent effects.

## 9. Fullbright-griefing kill switch

No current way to globally suppress fullbright rendering on in-scene objects.
Add a live toggle that, when disabled, force-clears the fullbright flag on
every in-scene object's texture entries (excluding the user's own avatar),
remembering the original value per-entry so it can be restored when
re-enabled. Anti-griefing utility against blinding fullbright-texture abuse.

**Why ranked #9:** real and useful, but a safety/anti-griefing utility rather
than a rendering technique — lowest priority of the confirmed items here.

## Note on future motion blur design (item #2)

If/when motion blur is implemented, consider exposing separate toggles for
blurring the user's own avatar versus other avatars (independent of the
general scene motion blur), so a photographer can keep themselves sharp while
background motion blurs, or vice versa. A worthwhile design detail to build
in from the start rather than retrofit.
