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
vendored under `.xo`, retaining its view-space depth preparation, horizon
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
| MXAO | Do not integrate | Primarily a ReShade/post-process implementation with its own compositing and indirect-light features; the public qt shader is marked all-rights-reserved. Native GTAO can use AyaneStorm's real G-buffer and cleaner lighting integration. |
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

The vendored `.xo` tree contains more than the core AO implementation.
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

This is not an "XeGTAO DoF" algorithm in the same sense as XeGTAO itself. It
is an unrelated Vanilla rendering-framework sample that happens to be present
in the vendored XeGTAO repository. It is also dormant prototype code: its
scene-view member and invocation are commented out, the whole invocation block
is compiled out, and `vaDepthOfField::Draw()` retains an unconditional debug
`assert(false)` after its first dispatch. Treat it as a design reference, not
as a tested replacement.

Compared with Firestorm's current DoF, its main image-quality advantage is the
explicit separation of foreground and background blur. Firestorm stores one
signed CoC in alpha, gathers both sides into one reduced-resolution image, and
then mixes that image back according to the destination pixel's CoC. Its far
gather rejects samples whose blur radius is too small, but its near gather has
no equivalent depth/plane discrimination. The Vanilla design instead creates
separate half-resolution near and far RGBA16F planes, uses their CoC masks to
select blur contributors, and composites far first and near second. This is
materially better at focused silhouettes and near/far overlap.

Firestorm remains better in other respects. Its CoC is derived from focus
distance, focal length, f-number, FOV and output resolution, with smooth focus
transitions and established Phototools controls. The Vanilla sample uses an
artist-defined in-focus interval and independent linear near/far transition
ranges; its disabled integration even labels that setup as not physically
correct. It also requires four half-resolution RGBA16F working images plus a
full-resolution R8 CoC image and runs a split pass, five far-blur passes, three
near-blur passes and a resolve. Firestorm uses fewer stages and permits a
0.25-1.0 resolution scale, although its variable-radius gather can become
expensive at large CoC values. Performance therefore needs measurement rather
than assuming either implementation wins.

The recommended upgrade is a hybrid: retain Firestorm's physical CoC/focus
model, controls and focus targeting, but feed the signed CoC into a separately
ported near/far split, blur and ordered resolve. Provide a fragment/FBO backend
for macOS and optionally a compute backend elsewhere. Neither design by itself
solves partially transparent hair, glass or particles represented by multiple
depth layers; that remains an OIT integration problem.

### qt ADoF comparison

The vendored `.qt dof` is not directly usable viewer code. It offers the
richest photographic/artistic bokeh controls of the three implementations:
configurable polygon vertex count, roundness, rotation, anamorphic ratio,
highlight emphasis, optional optical vignetting, CoC-linked chromatic aberration,
autofocus and temporal focus smoothing. Its main gather also scales its polygonal
ring count with CoC and follows with depth/CoC-weighted horizontal and vertical
Gaussian passes.

Its edge treatment is more developed than Firestorm's current single gather.
The CoC preparation conservatively chooses nearby minimum depths, performs a
depth-weighted four-sample color reduction, and the bokeh and Gaussian gathers
reject contributors whose absolute CoC is too small for the current radius.
These measures reduce focused-background leakage around foreground edges.
However, the blur uses `abs(CoC)` and keeps near and far content in the same
RGBA8 buffers. Consequently an out-of-focus foreground and an out-of-focus
background can still contribute to one another when both have sufficiently
large CoC. The Vanilla split-plane design remains the sounder basis for
foreground/background occlusion and ordered compositing.

qt is also less suitable as AyaneStorm's focus/lens model. Its CoC is an
artistic normalized-depth curve controlled by hyperfocus plus independent near
and far curve values, rather than Firestorm's focal-length/f-number/FOV model.
Its autofocus estimates one focus depth from a 10x10 screen-space sample grid;
that is useful to a generic injector, but the viewer already knows its selected
object, pointer ray, mouselook target and locked focus point. Preserve those
viewer-native controls instead.

Cost is highly variable. At the default six-sided shape and quality five, the
maximum main bokeh gather is about 90 taps per reduced-resolution pixel. The
quality control can raise that into the thousands in its extreme configuration,
in addition to an expensive full-resolution CoC preparation, two Gaussian
passes, combine and optional chromatic-aberration pass. Default blur resolution
is half width and height, but both working textures are allocated as
full-resolution RGBA8. It needs profiling against Firestorm; it is not an
automatic performance improvement.

The preferred AyaneStorm design remains Firestorm's physical/viewer-native
focus model plus Vanilla's near/far split architecture, augmented with selected
qt mechanisms: its conservative depth-aware CoC preparation, configurable
aperture kernel and highlight modes, Gaussian bleed rejection, optical
vignetting and DoF-linked chromatic aberration. Do not port qt wholesale:
retain signed near/far classification through the blur instead of its shared
`abs(CoC)` path, use appropriate HDR render-target formats, and expose bounded
quality presets rather than qt's potentially extreme raw sample controls.

### CinematicDOF comparison

The vendored `.ox/Shaders/CinematicDOF.fx` v1.2.10 is the strongest
complete implementation reference reviewed here. It keeps a full-resolution
signed R16F CoC (negative near, positive far), builds an expanded/blurred near
CoC coverage mask, performs distinct half-resolution far and near disc gathers,
tent-filters the far result, and resolves original/far first with the near plane
over it. This directly addresses the central weakness in Firestorm and qt's
shared blur: foreground blur must spread over background pixels without letting
background color incorrectly spread across focused foreground silhouettes.

Its near-plane handling is more developed than the Vanilla prototype. Several
minimum-CoC gathers propagate thin foreground coverage into neighboring pixels,
then a separable 18-offset Gaussian creates the near influence mask while
retaining the original CoC in a second channel. The near gather uses that mask
for both blur radius and resolve alpha. The far gather rejects negative-CoC
samples and weights accepted samples by their ability to cover the current
ring. Both paths use separate adjustable maximum radii and the combiner applies
far before near. This is a better practical starting point than porting the
Vanilla blur stages literally.

Other useful mechanisms are its optional same-plane preblur for undersampling,
9-tap tent upscale, CoC-aware highlight sharpening, highlight de/re-tonemapping,
anamorphic deformation, texture-defined aperture shapes, low-luminance dithering
and optional post-smoothing. Its CoC is lens-based (focus distance, focal length
and f-number), but AyaneStorm should still retain Firestorm's established CoC
calculation, resolution correction and viewer-native focus targeting. Because
AyaneStorm has real linear HDR buffers, do not port this approximation that
de-tonemaps an injected LDR backbuffer merely to reconstruct highlight range.

The implementation is expensive as written: 15 raster passes. At default blur
quality seven, the far gather takes up to 196 ring taps and the near gather up
to 252 taps per affected half-resolution pixel; quality 30 exceeds 3,000 taps
in each gather. Its declared working images nominally total about 31.5 bytes per
full-resolution pixel, roughly 249 MiB at 3840x2160 before shape/noise textures,
unless the host aliases lifetimes. Much of that comes from three "tile" R16F
images which are actually full resolution at the compiled `TILE_SIZE == 1`,
plus two full-resolution RGBA16F postprocess images. An AyaneStorm adaptation
must collapse/reuse targets and offer a few bounded presets rather than expose
this range directly.

One source issue should not be carried across: `PerformNeighborTileGather()`
uses `BUFFER_PIXEL_SIZE.x` for both axes of its neighbor offset; the Y term
should be derived from the Y pixel size. Review all resolution scaling instead
of translating the ReShade constants mechanically. Also verify separate rights
and attribution for any bundled custom bokeh images, which the README credits
to multiple artists. The shader code itself is permissively licensed: the file
contains BSD-style redistribution terms and the repository also includes an MIT
license; preserve the applicable notices and credits in derived files.

Revised recommendation: use CinematicDOF as the primary behavioral reference
for signed near/far classification, propagated near coverage, plane-specific
gathers and ordered resolve. Combine it with Firestorm's camera integration and
physical controls, a leaner render-target/pass plan informed by Vanilla, and
selected qt aperture/optical features. Implement the result as an AS-owned
module rather than a line-for-line ReShade translation.

### DoF implementation ownership and compatibility decision

Do not replace or progressively mutate Firestorm's existing blur and combine
shaders. Their single signed-CoC/color buffer is the architectural limitation
the new design must remove, and modifying them would increase upstream merge
risk while eliminating a known compatibility baseline. Keep the complete
legacy renderer available as a `Standard/Firestorm` mode.

Implement the advanced renderer in new AS-owned C++ and shader files, with its
own render targets, signed-CoC preparation, near-coverage propagation, separate
near/far gathers and ordered resolve. Reuse behavior rather than the legacy
rendering implementation: the existing master enable, focus-point selection and
transition, focus lock/crosshair, focal length, f-number, FOV and maximum-CoC
controls should retain their meanings in both modes.

The minimal upstream hook belongs inside `LLPipeline::renderDoF()` after its
focus interpolation and physical lens constants have been calculated but before
the legacy CoC pass. Pass those calculated inputs, source/destination/depth
targets and the screen triangle to a transactional
`ASDepthOfField::render(...)`. A successful advanced render returns immediately;
when the advanced mode is disabled, unsupported, incomplete or cannot allocate
its targets, execution falls through to the untouched legacy passes. This keeps
the rendering backend fully AyaneStorm-owned without duplicating or relocating
Firestorm's fragile camera/focus-selection code.

Add a mode selector rather than another master checkbox. Initially default it
to the legacy renderer until shader compilation and runtime comparisons pass on
all platforms; the advanced renderer can become the default later without
removing the fallback. Allocate its resources only while selected. Advanced-
only controls should cover bounded quality, near/far maximum radius, aperture
shape/highlight treatment, optical vignetting and DoF-linked chromatic
aberration; ordinary users should not need to retune the established lens and
focus controls when switching modes.

#### Initial AyaneStorm implementation (2026-09-22)

The first implementation now follows that ownership boundary. New
`asdepthoffield.{h,cpp}` code owns a four-stage renderer: full-resolution signed
R16F CoC, reduced-resolution far gather, reduced-resolution foreground
scatter-as-gather with coverage, and full-resolution ordered far-then-near
resolve. Its new GLSL uses ordinary fragment shaders and FBOs only, keeping the
baseline within macOS OpenGL 4.1. Quality is bounded at 16, 32 or 48 procedural
aperture samples; the initial controls also expose separate near/far radius,
blade count and roundness, rotation, anamorphic ratio, highlight weighting and
diagnostic views. `CameraDoFResScale` remains the shared resolution/performance
control. The legacy renderer is still the default and remains the transactional
fallback on shader or target failure.

The integration occurs after transparency has already been resolved and glow
combined, at the same `LLPipeline::renderDoF()` location used by the legacy
effect. Standard alpha, Exact OIT, AVBOIT and AYAstorm therefore all present the
advanced renderer with their final composited scene color. They also share the
existing post-alpha DoF depth pass in `LLDrawPoolAlpha`, which writes
alpha-tested transparent geometry into `deferredScreen` independently of which
alpha compositor produced the color. No advanced DoF code selects or bypasses
an alpha mode, and fallback re-enters Firestorm's original DoF passes with the
same source, destination and depth targets.

This compatibility contract does not create per-transparent-layer depth. Like
Firestorm DoF, the initial renderer sees one nearest accepted depth per pixel;
hair over glass or several intersecting translucent layers cannot each receive
independent CoC after their colors have already been collapsed. Exact OIT and
AVBOIT retain correct alpha composition before DoF, but fully layer-aware DoF
would require carrying color/depth/coverage out of each compositor and is a
separate integration project. Runtime acceptance must cover Standard, Exact
OIT, AVBOIT and AYAstorm with opaque focus, alpha-masked foliage/hair,
translucent glass and emissive transparency at near, focal and far depths.

`ASDepthOfFieldBackend` reserves an automatic backend choice, but this initial
milestone deliberately implements only the common OpenGL 4.1 fragment path. A
GL 4.3 compute path should be added only when it reduces measured gather cost
(for example through tile classification, compact dispatch and shared sample
reuse) while producing equivalent near/far masks and resolve output. Merely
moving the same tap loops into compute would add another platform path without
a demonstrated performance gain.

#### Advanced AyaneStorm depth-of-field design review (2026-09-22)

The quality review identified tile-reduced and dilated CoC bounds,
adaptive ring counts, a modified background ring accumulator, a three-depth-
layer foreground accumulator, content-aware foreground hole filling, a
CoC-moment-guided postfilter, software variable shading rates and a hybrid
gather/scatter path for exceptional highlights. Its aperture kernel also
corrects the non-uniform area density created when circular samples are radially
deformed into a polygon, and can vary tangential/sagittal shape across the
screen.

These are general rendering concepts which require independent AyaneStorm
design and validation against its own renderer.

One low-risk improvement has been independently derived for the portable
backend: AyaneStorm's uniform-angle polygon samples now receive the radial
mapping Jacobian weight (`boundary radius squared`). This produces equal-area
integration without ie's generated CDF/LUT, adds no pass or texture, and
remains ordinary OpenGL 4.1 GLSL. It also corrects the aperture rotation and
anisotropic coverage math already identified during the initial implementation.

Runtime diagnostics then exposed a viewer-integration defect in the initial
build: custom sampler names were discovered as GLSL uniforms but were not
assigned texture channels by `LLGLSLShader`, leaving CoC and layer samplers on
texture unit zero. The signed-CoC view consequently resembled scene color and
both layer views reproduced the unblurred input without a compile or link
error. The shaders now use distinct existing viewer-reserved sampler slots and
the C++ binds those slots by enum. One-time active/fallback log messages were
also added so future silent shader or target failures are distinguishable from
valid near-zero CoC.

The initial aperture-roundness default of `1.0` also made every selected blade
count mathematically circular, making the shape selector appear ineffective.
The default is now `0.35`, while an explicit Circle selection remains fully
circular; the UI states that rounding `1.0` intentionally returns any polygon
to a circle.

Large background radii in the first runtime build exposed coherent grid/band
artifacts from reusing one bounded aperture spiral at every gather pixel. The
portable path now varies only the sampling phase spatially while preserving the
actual aperture orientation. At radii above six pixels, resolve also applies a
small tent reconstruction over neighboring independently phased far gathers;
the blend increases gradually through eighteen pixels and leaves low-radius
detail and the foreground layer untouched.

The second runtime comparison established that signed CoC classification is
correct, but the initial near resolve was not a valid layer representation. It
forced full opacity from the center foreground pixel, stored unassociated color,
and composited it over the visible foreground source rather than a reconstructed
background. That necessarily produced a blurred but hard-edged cutout. The near
target now stores premultiplied color with coverage derived from the whole
aperture estimate, and high-radius coverage is reconstructed across neighboring
decorrelated gathers. The far pass also produces a conservative background
plate beneath foreground pixels by accepting only focal/far samples within the
foreground aperture. Resolve uses that plate only for a genuinely defocused
near layer, then performs premultiplied over-compositing.

Runtime alpha-mode results were consistent across the new hook: Exact OIT and
AVBOIT retained good transparent color composition, while Standard and
AYAstorm retained Firestorm's existing transparent cutout behavior. Thin hair
over an out-of-focus background remains blurred even when its face-adjacent
portion is focused. The cause is the common depth-only alpha pass: fragments
below its alpha threshold do not enter `deferredScreen`, so postprocess DoF sees
the background depth after transparency color has already been collapsed.
Lowering that threshold globally is not a solution because ordinary glass would
then write a misleading opaque foreground depth. Correcting this requires an
AyaneStorm transparent-DoF representation (coverage plus depth/layer data), not
another threshold tweak; Exact OIT/AVBOIT can source it from their retained
layers, while Standard/AYAstorm will require a bounded auxiliary capture.

The transparent-layer foundation uses AyaneStorm-owned auxiliary targets shared
by all four alpha modes. Opaque HDR color and depth are preserved before alpha
using OpenGL 4.1 framebuffer blits. An auxiliary replay accumulates transparent
coverage, while a separate pass records nearest transparent depth without
altering opaque depth.
The owned resolve now runs on the linear HDR scene before bloom and tone mapping;
the auxiliary captures cannot be composited correctly after the display transform.
CoC preparation retains effective, opaque and transparent CoC plus coverage as
separate channels. Opaque blur now follows opaque CoC even beneath in-focus
hair; transparent blur follows transparent CoC. Because the replay's RGB draw
order can disagree with the selected alpha compositor, the gather instead
derives an effective premultiplied contribution from final and opaque linear-HDR
colors plus captured coverage. Signed RGB is retained for custom darkening
blends. This preserved the actual compositor's color ordering but remained a
single transparent focal layer; overlapping transparent surfaces at different
depths were still approximated.

The owned rigged/world transparent split snapshots rigged coverage and depth,
captures world depth independently, records two transparent CoCs, and gathers
each stratum before depth-ordered HDR composition. The first runtime build
substantially improved avatar hair in front of glass and other transparent
scenery (`AyaneStormOS-Normal_iQpgIozSHk.png` and
`AyaneStormOS-Normal_m3scqj1Nzt.png`). A dark hair-on-hair patch remained in
the marked region of the second image. Multiple rigged depths within one
pixel are still represented by only one CoC; this needs a multi-depth design,
not a blend-threshold patch. The same limitation applies to multiple world
transparent depths. Check Standard, Exact-OIT, AVBOIT and AYAstorm separately.

### DoF regression and paused implementation state (2026-09-23)

**Runtime-confirmed update:** For `AyaneStormOS-Normal_NxwcjfS3CD.jpg`,
forcing `U_USE_OCCUPANCY` to zero in the transparent gather eliminated the
square artifacts (user: "no more squares"). The occupancy early-out caused
the reported squares; its precise logic defect remains undiagnosed. Keep it
temporarily disabled. Occupancy generation, resources, bindings and shader
code remain intact. This supersedes the partial-removal state and removal
instructions below, which do not describe the current source. Other DoF
limitations remain separate; this comparison does not establish their resolution.

- A trial that used the final HDR residual as rigged hair color where world
  coverage was absent introduced skin-colored holes in overlapping hair
  (`AyaneStormOS-Normal_xLRnRr0ZfI.png`). The replay's coverage does not
  necessarily equal the selected compositor's effective coverage, so
  subtracting the opaque face can contaminate the residual. That trial was
  removed from both transparent gather and resolve, and the removal was built.
- The subsequent build still had block-shaped dark patches in the back hair
  (`AyaneStormOS-Normal_Z5WeC7F3bu.jpg`). They disappear with DoF off at the
  same camera angle. This build still included the new 16-pixel occupancy
  pyramid, whereas the earlier visually better build did not. Occupancy is
  the leading suspect, not a proven root cause; the grid-shaped artifact
  supports testing it by removal.
- At pause, the occupancy pass/resource/bindings were removed from
  `asdepthoffield.cpp` only. The shader still contains `shadowMap0`,
  `use_occupancy`, `nearbyLayer` and its early-out; the now-unregistered
  `asDepthOfFieldOccupancyF.glsl` file remains. **No build or runtime test
  has occurred after this partial removal.** Finish removing the unused
  occupancy shader code and file before the next build. Do not disturb the
  rigged/world depth split or revert the earlier hair-color rollback.
- Then build once and compare the exact back-hair camera angle with DoF on/off
  and the hair-in-front-of-glass angle. If the blocks persist, inspect rigged
  coverage (debug 12), rigged signed CoC (10), and rigged far/near gathers
  (14/15) at the marked pixels; do not attribute them to occupancy without
  that comparison. If they disappear, design a different performance
  optimization only after the quality baseline is stable.
- High quality uses 96 aperture samples and, with layered transparency, two
  opaque plus four transparent gathers. Measure GPU time and memory on
  OpenGL 4.1 and newer hardware after visual correctness is restored.

Large-radius bokeh currently uses reduced-resolution stochastic aperture
gathers. A 48-sample high-quality gather left visible dotted hexagons around
small bright lights and stippled foreground hair. High quality now uses 96
samples, while the cheaper presets remain 16/32; premultiplied near/far
layers receive a nine-tap, radius-limited reconstruction before composition.
This is a quality/performance tradeoff, not a substitute for a future
motion-aware temporal or analytic highlight path. Measure high-quality GPU
time on the OpenGL 4.1 baseline and modern GPUs after runtime validation.
The acceptance target for isolated background lights is a continuous,
approximately uniform aperture disc with a clean selected outline, not a
collection of bright sample dots. Increasing the point-sample count and
screen-space reconstruction alone has not met this target. The next bokeh
stage should integrate a finite source footprint per aperture sample (or use
an equivalent CoC-aware prefiltered representation) before considering more
samples; preserve the aperture boundary and energy while doing so.

A stronger highlight path can rasterize each selected bright source as a
filled, antialiased aperture footprint in linear HDR, with its source color,
signed CoC, blade count, roundness, rotation and anamorphic ratio. Distribute
source energy across the footprint so overlapping discs accumulate radiance
without becoming opaque stickers; retain separate coverage for depth-aware
composition. Remove the selected highlight energy from the diffuse gather to
avoid a sharp duplicate underneath. Candidate selection must be stable under
camera movement and include eligible transparent highlights, not just opaque
pixels. An OpenGL 4.1 baseline can use instanced rasterization/transform
feedback; a newer optional path can compact candidates with compute shaders.
These are design options, not implemented or runtime-validated. Khronos API
references: [instanced drawing](https://wikis.khronos.org/opengl/GLAPI/glDrawElementsInstanced),
[transform feedback](https://wikis.khronos.org/opengl/Transform_Feedback),
[blending](https://wikis.khronos.org/opengl/Blending), and
[compute shaders](https://wikis.khronos.org/opengl/Compute_Shader).

The next highest-value quality work is foreground-edge reconstruction.
Coverage alone is insufficient when an out-of-focus foreground silhouette
reveals background that was never sampled. A robust solution classifies
contributions relative to the center into multiple CoC layers,
estimates missing background coverage, and obtains a depth-valid replacement
before blurring it. AyaneStorm should pursue a smaller independent variant: a
near-coverage/occlusion prepass plus conservative background hole-fill input,
retaining the existing separate near/far targets. This would most improve hair,
foliage and avatar silhouettes. It must still consume the shared finalized
color/depth contract so Standard, Exact OIT, AVBOIT and AYAstorm behave alike.

For the future GL 4.3 backend, CoC tile bounds are more valuable than merely
moving the current loops into compute. Reduce each tile to intersectable near,
far and minimum-absolute CoC, conservatively dilate those bounds, skip absent
planes, select work/sample density from blur size, and force full rate for
mixed-sign tiles. That provides an actual compute advantage while the GL 4.1
fragment path remains authoritative. A variance-aware finishing filter is also
worth testing: retain first and second CoC moments with blurred color, then
smooth sparse samples only when neighboring blur-radius distributions agree.

Hybrid highlight scattering is visually attractive but lower priority. It
detects statistically exceptional highlights, removes their energy from the
gather source and adds aperture-shaped sprites so bright bokeh is not diluted.
An AyaneStorm version would need independent thresholds, strict energy
conservation, bounded sprite counts and careful HDR/post-tonemap placement.
Screen-position-dependent radial/tangential aperture deformation is a cheaper
artistic feature and can be added later as a physically motivated cat-eye or
Petzval control.

#### Remaining AyaneStorm DoF implementation roadmap

The current blade, roundness and rotation controls affect procedural gather
positions, but bounded independently phased gathers do not preserve a coherent
aperture outline around isolated highlights. Visible polygonal bokeh is
therefore not complete: a selected hexagonal aperture currently tends to look
like ordinary blur. Treat this as a missing image-synthesis capability, not a
control-tuning problem.

Complete the renderer in the following order:

1. Finish and validate the transparent-layer contract. Accumulated coverage
   must represent every contributing transparent fragment, while nearest
   transparent depth is used only for CoC. An in-focus resolve must reproduce
   the selected alpha compositor exactly. Exact OIT and AVBOIT should
   eventually export already-computed coverage/transmittance through a common
   interface; Standard and AYAstorm retain the OpenGL 4.1 auxiliary fallback.
2. Add coherent aperture-shaped highlight scattering. Select exceptional HDR
   highlights, subtract the same energy from the gather input, render a bounded
   number of analytic aperture sprites, and add that energy back during
   resolve. Blade count, roundness, rotation and anamorphic scaling must be
   plainly visible on suitable defocused lights without changing scene
   exposure.
3. Replace the simple foreground model with a compact multi-CoC-layer
   accumulator and depth-valid background hole fill. This is the main path to
   stable hair, foliage and avatar silhouettes without hard cutouts or leaked
   background plates.
4. Add adaptive work selection. Portable OpenGL 4.1 uses fragment/FBO tile
   classification and bounded quality tiers; the newer backend uses compute
   tile reduction/dilation, plane skipping and adaptive sample density.
5. Store first and second CoC moments and apply a radius-aware postfilter. It
   should close sparse sampling gaps while preserving aperture boundaries and
   mixed near/far edges.
6. Add optional spatially varying aperture deformation for cat-eye,
   astigmatism and Petzval-style bokeh after the central aperture response is
   correct.
7. Add optional DoF-linked longitudinal chromatic aberration and optical
   vignetting only after color/coverage conservation is proven.

The common path must remain OpenGL 4.1 and use fragment shaders, framebuffer
targets and ordinary blending. Compute shaders, image load/store, group-shared
reductions and compact dispatch require the newer backend; they are
optimizations, not dependencies of the effect. Software variable shading rate
belongs only in that backend unless a portable tile-resolution scheme proves
both visually equivalent and measurably faster.

No screen-space DoF can reconstruct arbitrary fully occluded background or an
unbounded stack of transparent surfaces from one resolved frame. AyaneStorm can
improve those cases by exporting renderer-owned layers and transmittance, but
must keep memory and layer counts bounded. The goal is stable, energy-conserving
real-time synthesis rather than an unbounded physical simulation.

The whole reference is unsuitable as AyaneStorm's common implementation. It is
compute-centric, uses many full-resolution RGBA16F images plus moment, tile,
atlas and mip resources, and its declared intermediates are approximately
0.75 GiB at 3840x2160 before external inputs and implementation overhead. It
also includes ReShade-specific focus estimation and nonlinear dynamic-range
packing that AyaneStorm should not inherit: viewer-native focus is more stable,
and native render-pipeline placement should determine highlight handling.

### Directional Depth Blur

`.ox/Shaders/DirectionalDepthBlur.fx` is useful as a separate artistic
Camera Effect, not as an extension or replacement for AyaneStorm's existing
reprojection-based camera motion blur. It applies one-sided streaks only beyond
a focus depth. Its parallel mode uses a fixed screen direction; its focus-point
mode produces radial/zoom-like strokes toward or away from a selected point and
can mask the effect with a rotated, deformed and feathered ellipse. Highlight
gain and a focus-point color treatment provide additional stylization. None of
this depends on actual camera or object velocity, so a static scene remains
blurred.

The source implementation uses four passes and declares two full-resolution
RGBA16F targets plus one full-resolution R16F mask, nominally about 18 bytes per
pixel or 142 MiB at 3840x2160. Its `ScaleFactor` stores a reduced copy inside a
full-size texture rather than allocating a smaller target, while the blur still
runs at full output resolution. Sample count is also indirectly
resolution-dependent: it iterates over `length(screenSize) * BlurLength` with a
step of `1 / BlurQuality`, which is about 110 taps at 1920x1080 and 220 taps at
3840x2160 using the defaults. It only tests whether each sample lies beyond the
focus plane; it does not reject discontinuities between different far-depth
surfaces. Its highlight de/re-tonemapping is another workaround for ReShade's
LDR input and is unnecessary with native linear HDR.

An AyaneStorm implementation should be a new AS-owned module with bounded
quality presets and blur length expressed in pixels at a reference short-edge
resolution. Compute the elliptical focus mask analytically in the blur/combine
shader instead of allocating `texFilterCircle`; use the existing postprocess
ping-pong targets; allocate a genuinely smaller intermediate only for reduced-
resolution modes; and add relative linear-depth rejection using the established
motion-blur pattern. At full resolution the blur and depth transition can be
combined in one pass. Preserve explicit one-sided direction/flip controls, and
consider an optional symmetric mode. Place it after DoF and before AA, default
off, with interaction tests against camera motion blur.

### Depth Haze

`.ox/Shaders/DepthHaze.fx` is not atmospheric scattering. It is a small
depth-aware separable blur followed by two depth-dependent blends: distant
pixels receive more blurred color, then a selected fog color is added most
strongly around the vertical screen center and fades toward the top and bottom.
It therefore supplies a useful photographic "distance softening" look that is
different from Windlight haze, AyaneStorm horizon scattering and volumetric
lighting. It should be exposed as an optional Camera Effect, preferably named
`Depth Haze / Distance Softening` to avoid implying a physical atmosphere.

The reference runs two full-resolution nine-tap passes into RGBA8 buffers and a
full-resolution combine, nominally 8 bytes per pixel or 63 MiB at 3840x2160.
Its edge weight `(1 - abs(depth difference)) / distance * neighborDepth` uses
normalized depth, has no tunable depth sigma, and is not stable in world-space
terms across camera ranges. Its fixed four-pixel radius also becomes visually
smaller as resolution rises. The fog factor is a simple screen-Y triangle, so
it follows the image center rather than the actual world horizon, and the fog
output alpha accidentally takes the red color channel. Do not translate these
details literally.

Implement the useful concept with linear view depth, a relative bilateral
depth threshold, resolution-independent radius, HDR or existing postprocess
targets, and a reduced-resolution option. Separate `softening strength` from
`tint strength`; expose start/end distance rather than normalized injected
depth. A `screen band` mode can preserve the photographic look, while an
optional view-ray-elevation horizon mode would remain stable under camera pitch.
Apply scene haze before lens DoF and AA so DoF treats the softened/tinted scene
as its input. Keep it independent of environment haze settings and default off.

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
