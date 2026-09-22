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
capabilities that are currently fully absent (motion blur, GTAO) or broken
(glow threshold).

## 6. GTAO ambient-occlusion mode (retain existing SSAO as the inexpensive fallback)

**Recommendation after comparing the practical AO families:** implement GTAO,
not HBAO, as AyaneStorm's one new high-quality AO technique. Expose `Off`,
`Legacy SSAO`, and `GTAO`, with quality levels inside GTAO rather than adding a
long list of substantially overlapping algorithms. GTAO is the strongest fit
because it retains the depth/normal-only integration advantages of screen-space
AO while using a radiometrically derived horizon integral intended to approach
ray-traced ground truth. HBAO is its older conceptual ancestor, so implementing
both would buy little useful choice for the maintenance and UI cost.

This conclusion is also specific to AyaneStorm's renderer. The current AO is an
eight-tap fragment-shader estimate in `class1/deferred/aoUtil.glsl`; it is packed
into the green channel of `deferredLight` beside three shadow terms by
`class2/deferred/sunLightSSAOF.glsl`, then all four channels receive the same
two-pass plane-aware blur before AO modulates ambient/reflection-probe
irradiance in `class3/deferred/softenLightF.glsl`. A modern AO implementation
should be an AyaneStorm-owned `asambientocclusion` module with a dedicated
single-channel AO target and its own depth/normal-aware denoiser. Only small,
tagged selection/composite hooks should enter the shared pipeline and soften
shader. Do not force GTAO through the existing shared shadow blur.

The initial implementation should adapt the MIT-licensed Intel XeGTAO source
vendored under `.XeGTAO`, retaining its view-space depth preparation, horizon
evaluation, and edge-aware spatial denoise. AyaneStorm presently has no general
TAA history, so temporal reprojection should not be a prerequisite; XeGTAO
explicitly supports operation without TAA, with a fixed spatial noise pattern
and its spatial denoiser. GTAO itself does not require compute shaders: Intel's
implementation uses them as an optimization. Implement two interchangeable
backends: a GLSL 4.00 fragment/FBO path so GTAO works on macOS, Windows, and
Linux, plus an OpenGL 4.3 compute path closely adapted from XeGTAO for supported
hardware. Both must use the same settings, constants, resource formats, and
final visibility convention. Select compute automatically when supported and
otherwise select fragment; expose a developer-only backend override for visual
equivalence and performance testing, not two user-facing AO techniques.
Suggested user controls are AO radius, strength, and quality/denoise presets;
keep the auto-tuned heuristic constants compiled at Intel's defaults initially,
and keep the current SSAO controls active only in legacy mode.

### Concrete adaptation of the vendored XeGTAO source

Treat `XeGTAO.h` and `XeGTAO.hlsli` as the algorithm/reference source;
`vaGTAO.{h,cpp}` and `vaGTAO.hlsl` describe host orchestration and engine-facing
bindings but must not be copied as a rendering framework. Preserve the MIT
notice in derived shader/source files and use AyaneStorm-native naming and
resource management.

- Add a new `asambientocclusion.{h,cpp}` module and AyaneStorm-owned fragment
  and compute shaders behind one backend-neutral interface. Port only scalar
  visibility initially. Exclude the sample's ImGui, DXIL/DirectX abstractions,
  debug render target, generated-normal path, bent-normal path, auto-tuner, and
  reference ray tracer.
- Target OpenGL 4.0 / GLSL 4.00 for the baseline GTAO path (and therefore
  support macOS's OpenGL 4.1 ceiling). Express each stage
  as fullscreen fragment passes over FBO attachments; this needs no image
  load/store, SSBO, or compute support. The OpenGL 4.3 backend should use the
  existing compute-shader infrastructure and explicit
  `GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT` barriers
  between producer and consumer dispatches. Keep legacy SSAO for lower OpenGL
  levels and as the inexpensive mode, not merely as a macOS substitute.
- Reproduce the three logical stages: (1) linearize raw depth at full
  resolution, then render four weighted downsample passes into the remaining
  mip levels; (2) evaluate GTAO in a fullscreen fragment pass with MRT outputs
  for visibility and packed four-neighbour edges; and (3) run one or more
  fullscreen edge-aware denoise passes with ping-pong targets. Intel's compute
  shader creates all five depth levels in one dispatch and denoises two
  horizontal pixels per invocation, but these are optimizations rather than
  algorithmic requirements. The source presets are Low = 1 slice x 2 steps,
  Medium = 2x2, High = 3x3, and Ultra = 9x3; each step samples both directions.
  AyaneStorm additionally provides Cinematic = 18x4 for low-grain still
  photography at roughly 2.7 times Ultra's GTAO main-pass sampling cost. Start
  with High plus one denoise pass as the default, then tune from measured
  AyaneStorm GPU timings.
- Keep algorithm code and constants aligned between backends. Backend-specific
  code should be limited to texture access, output, group-shared depth-mip
  construction, two-pixels-per-invocation denoising, and host dispatch/draw
  orchestration. Add debug comparison modes that can render either backend and
  report GPU time; an optional absolute-difference view is preferable to judging
  screenshots by eye. The fragment backend is the correctness baseline because
  it is available on every target platform supporting GTAO.
- Allocate a five-level `GL_R16F` view-depth texture, `GL_R8` visibility
  working/output textures, and a `GL_R8` edge texture. Use regular GLSL `float`
  arithmetic first: HLSL `min16float` is a performance hint with no equally
  portable GLSL 4.00 spelling, while R16F storage retains the intended bandwidth
  saving. Add explicit 16-bit arithmetic only after cross-vendor profiling.
- Supply AyaneStorm's existing deferred normal attachment instead of generating
  normals from depth. Its `decodeNormal()` result is already a view-space
  normal. Adapt coordinate conventions deliberately: XeGTAO operates with
  positive view depth and top-left texture Y, while this OpenGL renderer uses
  negative view Z and bottom-left texture Y. Convert both reconstructed
  positions and normals consistently (equivalently, map viewer view space by
  `(x, y, z) -> (x, -y, -z)`) before applying the XeGTAO math. Derive and test
  OpenGL depth linearization rather than copying the DirectX projection-index
  formula from `GTAOUpdateConstants()`.
- Preserve the 64x64 `R16UI` Hilbert/R2 noise scheme, with temporal index zero
  until a true temporal consumer exists. GLSL 4.00 provides `texelFetch`,
  `textureLod`, integer texture sampling, and multiple render targets needed by
  this path. Prefer explicit `texelFetch` for centre/neighbour and denoiser
  reads where HLSL `GatherRed` lane ordering would be ambiguous; optimize to
  `textureGather` only after image-equivalence tests.
- Keep the final GTAO texture separate from `deferredLight`. In GTAO mode the
  sun pass must omit legacy `calcAmbientOcclusion()`, the existing shared
  shadow blur may continue processing the shadow channels, and
  `softenLightF.glsl` should sample the dedicated GTAO visibility when adjusting
  irradiance. This prevents AO from inheriting the shadow blur and allows its
  edge-aware denoiser to remain authoritative.
- Defer bent normals. They are valuable only after the reflection-probe ambient
  lookup is explicitly changed to consume them; merely computing and then
  discarding them adds about 25% to XeGTAO's documented cost.

### Why not add every named alternative

| Technique | Decision | Reason relative to GTAO in this viewer |
|---|---|---|
| HBAO | Do not add | Older horizon formulation; GTAO is the more accurate successor and serves the same role. |
| HBAO+ | Do not add | Good historical optimization, but NVIDIA's published package exposes DX11/DX12 binary-library integration, not a portable OpenGL implementation; its quality/performance niche is already covered by GTAO. |
| HDAO | Do not add | Legacy AMD/DirectCompute-era kernel; its later AOFX form is GCN/DX11-oriented and offers no compelling advantage over GTAO or CACAO. |
| VXAO | Reject | Voxelizes scene geometry and depends on the old VXGI/GameWorks approach. It is not a screen-space drop-in and would impose a major scene/pipeline and memory cost. |
| NNAO | Research only | The 2016 method bakes a trained 31x31 depth/normal model into shader filters. It is interesting but brings training-data/generalization and weight-asset maintenance risk without a demonstrated benefit over current GTAO implementations. |
| SSDO | Separate future GI feature | Directional occlusion plus a screen-space diffuse bounce is closer to limited SSGI than a scalar AO replacement. It needs radiance/color handling and should not compete in the AO selector. |
| GTAO | **Implement** | Best balance of physically grounded output, cross-vendor operation, tunable cost, and compatibility with existing depth/normal inputs. |
| LSAO | Do not add | Clever linear-complexity line sweeps, but awkward multi-direction whole-image passes and a 2013 obscurance model make it a poor trade beside GTAO. |
| DeepAO | Research only | Learned compute-shader pipeline and project-specific model/data burden; sparse reference implementation and no clear production advantage here. |
| CACAO | Benchmark later, at most | The only strong second candidate: MIT-licensed, optimized, adaptive, and offers five quality levels. However it is a many-pass compute implementation officially targeting DX12/Vulkan; an OpenGL port would require 4.3-class compute and would exclude macOS. Add it only if an instrumented GTAO prototype misses a defined frame-time target. |
| MXAO | Do not integrate | Primarily a ReShade/post-process implementation with its own compositing and indirect-light features; the public qUINT shader is marked all-rights-reserved. Native GTAO can use AyaneStorm's real G-buffer and cleaner lighting integration. |
| RTAO | Defer until renderer/API work exists | True scene-space quality, but requires ray-query/pipeline support plus maintained GPU acceleration structures. The OpenGL renderer has neither, so this is a renderer-backend project rather than an AO option. |
| AAO / ABAO | Do not add | Alchemy AO (AAO) is a fast older obscurance approximation; angle-based AO (ABAO) is likewise superseded for this use. If `AAO` meant adaptive AO/ASSAO, CACAO is its optimized descendant and is the relevant candidate instead. |

**Why ranked #6:** this is a substantial visual upgrade but SSAO already works.
GTAO replaces the former HBAO proposal because it offers a larger accuracy gain
for similar architectural effort and avoids maintaining two horizon-based modes.

**Primary references:** [original GTAO technical report](https://research.activision.com/publications/archives/atvi-tr-16-01practical-realtime-strategies-for-accurate-indirect-occlusion),
[MIT-licensed XeGTAO implementation and integration notes](https://github.com/GameTechDev/XeGTAO),
[AMD CACAO documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/combined-adaptive-compute-ambient-occlusion/),
[NVIDIA HBAO+ package/API description](https://github.com/NVIDIAGameWorks/HBAOPlus),
[NNAO paper](https://www.pure.ed.ac.uk/ws/portalfiles/portal/28369946/nnao_5_.pdf),
[LSAO paper](https://diglib.eg.org/server/api/core/bitstreams/786e1ab5-c669-48ac-9e99-56a1564edcad/content),
and [Alchemy AO](https://casual-effects.com/research/McGuire2011AlchemyAO/index.html).

## 7. SSAO sample count is hardcoded (existing SSAO is untunable)

`aoUtil.glsl:89` hardcodes `for (int i = 0; i < 8; i++)` for the SSAO sample
loop. Expose this as a live setting wired through a dedicated uniform into the
GLSL loop bound, so users can trade AO quality for performance without a
rebuild.

**Why ranked #7:** small, low-risk, high value-for-effort — a single new
uniform plumbed through the existing SSAO shader, no new algorithm. Distinct
from item #6 (GTAO): this is about tunability of the *legacy* SSAO fallback,
not the new high-quality mode. It remains a useful independent quick win.

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

## XeGTAO repository follow-on audit (2026-09-22)

The vendored `.XeGTAO` tree contains more than the core AO implementation.
AyaneStorm has already ported the important scalar XeGTAO pieces into
`asambientocclusion`: five-level weighted view-depth mips, Hilbert/R2 spatial
sampling, the radiometric horizon integral, packed depth edges, edge-aware
spatial denoising, and both fragment and compute backends. The remaining
interesting pieces are therefore follow-ons, not another GTAO rewrite.

### Best candidate: GTAO bent normals for directional probe lighting

XeGTAO can integrate a bent normal alongside scalar visibility. This records
the average unoccluded direction rather than only how much of the hemisphere is
visible. AyaneStorm's reflection-probe irradiance is already sampled by a
direction in `class3/deferred/softenLightF.glsl`, so using the denoised bent
normal for diffuse probe/sky irradiance could stop ambient light appearing to
come through the occluded side of corners and openings. Keep the geometric
normal for direct sun/moon lighting, BRDF terms, and shadowing.

This is the strongest unported XeGTAO feature, but it is not free: the source
documents roughly 25% extra GTAO cost, the working/final AO target must carry a
direction plus visibility instead of one `R8` scalar, and the denoiser must
filter and renormalize that direction. A portable representation should be
chosen for both fragment and compute backends (for example octahedral `RG` plus
visibility, rather than blindly copying HLSL's packed `R32_UINT` path). Add it
as a higher quality option after scalar GTAO is runtime-proven, not as the
default.

#### Implementation status (2026-09-22)

Implemented as the opt-in `RenderGTAOBentNormals` GTAO mode for both fragment
and compute backends. The AO working targets switch from `R8` visibility to
`RGBA8` encoded bent direction plus visibility, and the edge-aware denoiser
filters and renormalizes both. Deferred sky ambient plus PBR and legacy diffuse
probe irradiance use the result; direct, shadow, SSR, hero-probe, and glossy
directions retain the geometric normal. `RenderGTAOBentNormalInfluence`
provides a 0–1 blend for tuning, exposed beside the checkbox in the enlarged
Photo Tools GTAO panel.
The feature defaults off pending runtime performance and image validation.

### High-value foundation: Intel TAA, but only after real motion vectors

`IntelTAA.hlsli` contains a serious temporal resolve: projection jitter,
depth-based history rejection, velocity confidence, YCoCg variance clipping,
five-tap bicubic history sampling, neighbourhood recovery, and longest-velocity
selection. A correct TAA foundation would improve geometric aliasing and let
GTAO animate its Hilbert/R2 sequence across frames for temporal supersampling;
SSR and volumetrics could eventually use the same history infrastructure.

Do not port it on top of camera reprojection alone. The current
`asmotionblur` reconstructs camera velocity from depth and explicitly has no
per-object velocity buffer. Second Life has moving avatars, rigged meshes,
texture animation, and alpha surfaces, so camera-only TAA would ghost them.
The prerequisite is a maintained velocity attachment covering static,
skinned, and otherwise moving geometry, plus previous transforms and robust
history invalidation. Until then, GTAO's fixed frame index is correct; merely
animating its noise would replace stable spatial noise with visible shimmer.

### Concrete source for backlog item 3: split-plane depth of field

`vaDepthOfField.hlsl` is a more complete design reference than a single larger
blur shader. It computes near and far circles of confusion separately,
performs CoC-weighted downsampling, blurs near and far planes independently
(including a Poisson/bokeh kernel), and resolves them in depth order. That
architecture directly addresses background color bleeding across focused
foreground silhouettes and should inform item 3 above.

Reuse the architecture, not the DirectX compute wrapper verbatim. A portable
AyaneStorm implementation needs a fragment/FBO path for macOS and should start
with separate half-resolution near/far targets and a CoC target. This is a
larger but cleaner upgrade than the previously suggested one alternate
fragment shader.

### Small optional addition: Lottes and Uchimura tonemappers

`vaTonemappers.hlsli` includes compact MIT-licensed Lottes and Uchimura curves.
AyaneStorm currently exposes Khronos Neutral and ACES Hill, so these would add
genuinely different highlight/contrast responses at little shader cost. They
are photography choices rather than quality fixes and would require updating
the tonemap selector and translated UI labels, so rank them below bent normals
and DoF.

### Useful methods, not immediate features

- XeGTAO's ray-traced reference plus parameter auto-tuning is a good validation
  methodology. Rebuilding its scene ray tracer inside the viewer is not worth
  the integration cost, but representative AyaneStorm captures should be used
  when tuning radius, falloff, power, and quality instead of tuning one scene by
  eye.
- The depth-aware weighted mip filter is a useful pattern for bandwidth-bound
  screen-space effects. AyaneStorm already uses it for GTAO. Do not reuse that
  exact AO mip chain as SSR Hi-Z data: SSR needs conservative hit-testing
  semantics, while XeGTAO deliberately computes a radius-dependent weighted
  average.
- Half-precision arithmetic gives XeGTAO a documented 5-20% gain on some
  hardware, but portable GLSL 4.00 has no equivalent to HLSL `min16float`.
  Retain `R16F` storage and only add explicit 16-bit math after extension and
  cross-vendor profiling.

### Not worth importing now

| Source component | Decision | Reason |
|---|---|---|
| `vaCMAA2.hlsl` | Skip | AyaneStorm already has FXAA and SMAA. CMAA2 needs a compute-only multi-buffer/indirect-dispatch integration for another spatial AA option, while TAA would provide the missing capability. |
| Filament cloth/subsurface shaders | Defer | The formulas are interesting, but Second Life materials do not provide the required cloth/subsurface model, thickness, power, and color semantics. Applying them heuristically would mis-shade existing content. |
| `vaIBL.hlsl` SH pipeline | Skip for now | AyaneStorm already generates and samples irradiance/radiance probe arrays. Replacing that system with spherical harmonics is a renderer project with no demonstrated benefit; bent normals can consume the current directional probe lookup directly. |
| XeGTAO normal-from-depth pass | Skip | AyaneStorm already has a deferred view-space normal attachment, which is more faithful than reconstructed depth normals. |
| Thin-occluder compensation | Leave disabled | XeGTAO itself disables it by default after auto-tuning found only a small, scene-dependent improvement; extra slices provide the preferred mitigation already used by AyaneStorm's quality presets. |
