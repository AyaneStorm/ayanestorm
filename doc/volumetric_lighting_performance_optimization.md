# Volumetric Lighting Performance Optimization

## Current Cost Model

### Measured 3440x2440 Case

The test resolution contains 8.39 million display pixels. A controlled capture
from one unchanged camera position with local lights explicitly disabled gave:

| Mode | FPS | Frame time | Increment over disabled |
|---|---:|---:|---:|
| Disabled | 55 | 18.18 ms | -- |
| Normal | 49 | 20.41 ms | 2.23 ms |
| High | 34 | 29.41 ms | 11.23 ms |

High adds approximately five times Normal's incremental frame cost in this
capture. This is directionally consistent with its theoretical increase: four
times as many raymarch pixels and twice the maximum steps, partially offset by
High skipping Normal's expensive bilateral upsample. It strongly identifies the
coupled full-resolution/32-step directional raymarch as the principal problem.

An earlier capture reported 70 FPS disabled, 45--50 Normal, and 25--30 High.
Because its disabled baseline differs substantially, do not subtract or compare
absolute milliseconds across the two captures. Viewer, simulator, scene, or GPU
state changed despite the nominal location. The controlled 55/49/34 sequence is
the current primary result.

The region contained no local lights in either case. With no qualifying nearby
lights, `renderLocalLights()` returns before binding the target or submitting a
draw, so the enabled setting itself contributed essentially no GPU work. The
measured deltas cover directional raymarch, transparency atlas when demanded,
and composite.

The directional pass is the primary suspect, but the existing single GPU zone
does not distinguish raymarch, transparency atlas, local lights, and composite.
Measure those four stages separately before changing quality.

Normal renders one quarter as many raymarch pixels as the display and permits
up to 16 steps. High renders every display pixel and permits up to 32 steps.
High therefore schedules up to eight times Normal's directional raymarch work:
four times the pixels and twice the steps. The observed 25% versus 50% FPS loss
is consistent with a GPU-bound pass whose cost is partially hidden by other
frame work; it does not mean High itself costs only twice as much GPU time.

One step is also much more expensive than one shadow lookup. It calls the
general-purpose `sampleDirectionalShadow()` surface-lighting function. Its PCF
routine performs five comparison samples, and cascade transition regions can
evaluate two cascades. A 16-step ray can consequently issue roughly 80 shadow
comparisons, or more in cascade overlaps. At 2560x1440 Normal, the worst case is
about 74 million shadow comparisons before the atlas and composite. High can
approach 590 million. Adaptive near-geometry step counts reduce these maxima,
but sky and distant geometry retain them.

The half-resolution composite is not free. Every full-resolution destination
pixel reconstructs center depth, reconstructs four tap depths, decodes a center
normal and four tap normals, evaluates four exponentials, then reads four
scatter taps. High avoids this depth-aware upsample, so its extra raymarch cost
is partly offset by a cheaper composite.

The transparency atlas covers one quarter-screen pixel count in total. Each
atlas pixel performs one directional shadow call, plus one previous-integral
read for slices after the first. It also incurs 16 draw calls and FBO attachment
changes. It is demand-gated already, but common foliage, water, fullbright, or
alpha content likely keeps it active.

## Highest-Value Optimizations

### 1. Add a volumetric-specific shadow sampler

Do not call the five-tap, surface-oriented PCF routine at every ray step.
Volumetrics already receive spatial filtering from half-resolution rendering,
per-pixel march jitter, integration along the ray, and the bilateral upsample.
A dedicated sampler should retain the same cascade matrices, clipping, and
cross-fade, but use one comparison tap per selected cascade. Use a stable
screen/step-dependent sub-texel rotation so neighbouring pixels collectively
sample the existing PCF footprint. If one tap is visibly noisy, use two taps
before considering the full five.

This is the best first lever: it can remove about 60–80% of raymarch shadow
texture fetches without changing resolution, step positions, scattering math,
or shadow-map resolution. Apply the same sampler to the atlas, whose output is
already spatially and depth-interpolated by consumers. Keep the upstream
`shadowUtil.glsl` function unchanged; place the specialized function in an
AyaneStorm-owned shader include or the two AS shaders.

Validate roofline shafts, foliage shadows, cascade boundaries, camera motion,
and sun/moon motion. A single static tap would shimmer; decorrelation is part of
the optimization, not optional polish.

### 2. Stop coupling High resolution and sample count

The current High switch combines two independent quality increases and creates
an eightfold work jump. Test these modes independently:

- half resolution, 16 steps: current Normal;
- half resolution, 32 steps: isolates integration quality;
- full resolution, 16 steps: isolates edge/spatial quality;
- full resolution, 32 steps: current High.

Full-resolution 16-step is the strongest likely replacement for current High.
It retains High's silhouette and fine-ray resolution at half its march cost.
Half-resolution 32-step is another useful comparison at only twice Normal's
step cost and one quarter of High's theoretical work. The correct choice should
come from side-by-side captures, not the existing combined switch.

### 3. Simplify the Normal upsample

The current 2x2 bilateral filter is unusually expensive for a post effect. Test
a depth-only 2x2 filter first: remove all five normal reads/decodes and the four
`normalSimilarity()` evaluations. Volumetric scatter is low-frequency; depth is
the guide that prevents foreground/background light bleeding. Normal rejection
inside a continuous surface can suppress valid smooth scatter and may not buy
visible quality.

Next replace four exponential depth weights with a cheaper monotonic weight or
select the nearest-depth source tap, then spatially blend only taps within a
relative-depth threshold. Preserve the current bilinear fallback for subpixel
surfaces. This optimization affects Normal only and may materially reduce its
25% toll even after the raymarch is improved.

### 4. Reduce steps by contribution, not only endpoint distance

The current adaptive count scales only with ray length, so most sky pixels use
all 16 or 32 steps. Use an explicitly tested quality table rather than a linear
maximum: for example 8/12 Normal and 12/16 High after adopting the specialized
shadow sampler. The existing interleaved jitter makes reduced counts much less
prone to fixed banding.

Also stop marching once the remaining maximum possible contribution is below a
small display-space threshold. Because Beer-Lambert attenuation is analytic,
the upper bound of the unvisited integral can be computed without sampling it.
This helps higher density values; it will save little at the default density
unless the threshold is too aggressive, so it is secondary to reducing PCF.

### 5. Render local lights as bounded screen-space volumes

The current local-light shader is a full-target draw that loops over every
selected light for every pixel. Most pixel/light pairs miss, but still pay ray
setup and sphere-intersection work. Draw one conservative screen-space bound per
light (or submit a compact tile light list) so pixels outside that light's
projected sphere execute no work. Additive accumulation preserves the existing
visual model. Lights intersecting the near plane need a full-screen or clipped
bound fallback.

Before changing submission, simplify the eight samples analytically. The line
integral of the current radial falloff may be approximated from chord length and
a small fixed quadrature of two or four samples. Replace variable `pow(x, p)`
with explicit inexpensive cases or a fitted polynomial for the supported
falloff range. Test 2, 4, and 8 samples in isolation. At this resolution, local
lights should not share High's full-resolution target automatically; half-
resolution local scatter composited into the full-resolution directional target
is likely visually indistinguishable because unshadowed fog volumes are smooth.

## Larger Optimization: Temporal Accumulation

Temporal reprojection is the route to a much larger quality-per-sample gain:
render 4–8 jittered steps per frame and accumulate shadow visibility/scatter
over time. It requires a history target, previous view-projection transform,
depth disocclusion rejection, camera-cut invalidation, and rejection or rapid
adaptation when the light direction or shadow maps change. Without these, it
will ghost around avatars, moving foliage, and newly revealed geometry.

This is likely worthwhile only after the specialized shadow sampler and quality
decoupling. Those are much smaller changes and attack demonstrable redundant
work. Checkerboard rendering without valid temporal reconstruction is not
recommended because moving shafts will sparkle or form holes.

## Secondary Levers

- Replace per-pixel `acos()` followed by `cos()` in the disc-width phase setup
  with the equivalent cosine-angle identity and a branch at
  `dot(ray_dir, light_dir) >= cos(radius)`. This removes two transcendental
  operations per raymarch pixel, but shadow fetches dominate.
- Precompute phase terms and moon tint/light colour on the CPU where practical.
  They are invariant over the march and already outside its loop, so savings
  are modest.
- Test an `R11F_G11F_B10F` directional target instead of `RGBA16F`; directional
  alpha is not consumed. This reduces target and composite bandwidth. Confirm
  `LLRenderTarget` support and local-light blending before adopting it.
- Profile atlas submission overhead. If it matters, update only a subset of
  depth slices per frame with temporal invalidation, or replace the 16 serial
  ping-pong draws with a layered/prefix design. Do not reduce atlas refresh
  blindly: stale transparent scatter is conspicuous during motion.
- The local-light pass is disabled by default. When enabled, it loops over all
  selected lights for every target pixel; screen-space light bounds or tiled
  light lists are the meaningful optimization, not small arithmetic changes.

## Measurement Plan

Add GPU timing zones around directional raymarch, atlas, local lights, and
composite. A CPU submission timer cannot measure shader completion. Capture GPU
milliseconds, not only FPS, at fixed resolution and camera pose for:

1. feature disabled;
2. Normal with atlas inactive and active;
3. High with atlas inactive and active;
4. the four independent resolution/sample combinations;
5. specialized shadow sampling at 1, 2, and 5 taps;
6. full bilateral, depth-only, and nearest-depth upsample.

Use scenes containing open sky, nearby opaque geometry, dense alpha foliage,
water, a cascade boundary, and a moving avatar. Report median and 95th-percentile
GPU frame time over at least several hundred frames. The first implementation
target should be the volumetric-specific 1/2-tap shadow sampler plus decoupled
High quality; together they have the clearest path to a large reduction while
retaining the current spatial resolution and scattering model.

# Lossless Volumetric Lighting Optimization

## Summary

Preserve the complete current visual workload:

- Normal: half-resolution, up to 16 steps.
- High: full-resolution, up to 32 steps.
- Preserve five PCF texture operations, cascade transitions, target formats, atlas resolution, and bilateral upsampling.
- Optimize redundant arithmetic and analytically replace only work whose result is already predetermined.
- Target at least 40 FPS at 3440×2440 against the controlled 55/49/34 baseline.

## Implementation Changes

### Profiling

Add nested Tracy GPU zones inside the existing volumetric zone:

- `AS volumetric directional`
- `AS volumetric atlas`
- `AS volumetric local lights`
- `AS volumetric composite`

Keep scopes directly around GPU submission. Do not add synchronization, timer queries, or periodic logging.

### AS-owned directional-shadow utility

Create a fragment compilation unit attached only to the directional and atlas programs. Multiple fragment objects are already supported by `LLGLSLShader::createShader()`.

Implement a specialized function that reproduces `sampleDirectionalShadow()`:

- Use the same four shadow samplers, matrices, clip planes, PCF coordinates, five texture operations, weights, bias and cascade transitions.
- Accept an already-normalized active light direction.
- Remove the repeated per-step light normalization.
- Remove the surface-normal dot product and offset calculation: volumetric callers pass the light direction as the normal, making the dot product one and the offset zero.
- Preserve the far-cascade fade term and division by accumulated cascade weight.
- Preserve exact boundary comparisons, including the strict `z > -shadow_clip.w` test.
- Leave upstream `shadowUtil.glsl` unchanged and give the new function a distinct AS-specific name.

Attach the utility before linking both AS shadow-consuming programs. Keep the existing `hasShadows` feature so shadow textures and common uniforms remain bound normally.

### CPU-precomputed invariants

In the AS module, calculate once per draw and upload:

- Normalized active sun or moon direction.
- Active light colour after moon horizon tint and phase illumination.
- `sin()` and `cos()` of the fixed celestial angular radius where useful.

Use these uniforms in the raymarch and atlas. Retain existing uniforms needed by other attached shader utilities.

### Directional raymarch arithmetic

Without changing sample count or positions:

- Calculate `length(ray_end)` once.
- Replace the `acos()`/`cos()` disc clamp with:
  - Return cosine one inside the angular radius.
  - Otherwise use `cos(a-r) = cos(a)cos(r) + sin(a)sin(r)`.
  - Derive `sin(a)` using `sqrt(max(1-cos²(a), 0))`.
- Calculate the first sample distance and fixed distance increment once.
- Calculate the first Beer–Lambert attenuation and fixed decay once.
- Advance distance, sample position, and attenuation by recurrence.
- Preserve the current jitter, visibility validation, accumulation semantics, phase function, brightness normalization, and output clamps.
- Handle zero density explicitly so the attenuation recurrence remains exactly one.

### Predetermined far-tail accumulation

For each view ray:

- If `ray_dir.z >= 0`, do not apply this optimization.
- Otherwise derive the distance where the ray reaches `-shadow_clip.w`.
- March normally while sample `z` satisfies the existing shadow-enabled condition.
- Once a sample and every later sample are beyond that boundary:
  - Add the remaining count directly to accumulated visibility.
  - Add the remaining discrete attenuation terms using a geometric series.
  - Handle decay equal or extremely close to one without division instability.
  - Exit the loop.
- Do not use an approximate contribution threshold or infer visibility from earlier samples.
- If the boundary lies beyond the 128 m volumetric range, execute the original complete march.

For the atlas:

- Use the specialized five-tap sampler for shadow-relevant segments.
- For a sample beyond `-shadow_clip.w`, use the existing predetermined visibility of one without invoking the sampler.
- Preserve the 16 slices, jittered sample distance, cumulative ping-pong integral, and atlas outputs.

### Repository constraints

- Confine substantive functionality to AS-owned code and the new AS shader utility.
- Apply ownership tags to any necessary `ll*`/`fs*` changes.
- Use `chanayane@firestorm` attribution.
- Do not modify user-facing quality settings.
- Do not bump the shader version during development.
- Do not alter local-light rendering in this pass.

## Validation

The user builds and tests.

Benchmark the unchanged camera position at 3440×2440 over several hundred frames:

| Mode | Existing baseline |
|---|---:|
| Disabled | 55 FPS / 18.18 ms |
| Normal | 49 FPS / 20.41 ms |
| High | 34 FPS / 29.41 ms |

Record total FPS and all four GPU-zone timings with:

- Atlas inactive
- Atlas active
- Shadow far plane below 128 m
- Shadow far plane at or beyond 128 m

Validate stills and motion around:

- Every cascade transition
- Roof and window shafts
- Fine foliage shadows
- Open sky and horizon
- Near opaque geometry
- Camera rotation and translation
- Moving avatars and foliage
- Sun and moon transitions
- Transparent foliage, glass, fullbright and water
- Existing volumetric debug modes

Acceptance requires:

- High reaches at least 40 FPS in the controlled scene.
- No reduction in ray steps, PCF taps, spatial resolution, atlas precision, or upsample quality.
- No new banding, shimmer, cascade seams, shadow softness changes, or transparent/opaque mismatch.
- Output is current quality or better.
- Normal does not regress.

If the target is missed, retain only changes that measurably reduce GPU time and pass visual validation. Report the stage timings and stop; do not introduce lower-quality sampling. Use the measured bottleneck for a separately approved architectural phase.

## Assumptions

- Small floating-point differences from mathematically equivalent recurrence are acceptable only when they create no visible difference or improve stability.
- The exact far-tail optimization may provide no benefit when the shadow range covers the entire 128 m march.
- The largest guaranteed changes are removal of repeated normalization, surface-only shadow arithmetic, transcendental work, and per-step exponentials; reaching 40 FPS is a measured target, not assumed.
- Local lights contributed no GPU work in the controlled scene.

## First Lossless Implementation Result

Runtime validation after fixing the standalone shadow utility's missing
`SUN_SHADOW` compilation context restored the rays and debug modes 2/3. The
controlled result was 58 FPS disabled, 50 FPS Normal, and 35 FPS High. This is
17.24 ms, 20.00 ms, and 28.57 ms respectively: Normal adds 2.76 ms and High
adds 11.33 ms.

The earlier controlled deltas were 2.23 ms Normal and 11.23 ms High. The High
cost is therefore unchanged within measurement noise. Removing repeated
normalization, surface-only offset work, per-step exponentials, and phase
transcendentals did not expose a measurable gain; their ALU cost was hidden by
the retained five-operation PCF shadow sampling. The 40 FPS acceptance target
was not met. Further lossless work must first isolate directional, atlas, and
composite GPU time; more small arithmetic rewrites are not justified by this
result.

### Non-Tracy GPU Diagnostics

The AS module now contains a four-query asynchronous `GL_TIME_ELAPSED` ring per
stage. Results are polled only after later frames report them available, so the
diagnostic never blocks waiting for GPU completion. Every 120 completed
composite samples it writes one `Volumetric GPU timing average` line containing
directional, atlas, local-light, and composite milliseconds plus the independent
sample count for each stage. This differs from Snow's `steady_clock` logging:
Snow needed CPU stage time, while volumetric shader execution requires GPU timer
queries to avoid measuring only OpenGL submission time. Set
`AS_VOLUMETRIC_PERFORMANCE_LOGGING` to zero after diagnosis.

### Measured GPU Stage Breakdown

The asynchronous timer capture produced stable Normal directional samples near
1.53 ms and stable High samples near 9.73 ms. Normal atlas production was about
0.28 ms and composite about 0.125 ms; High atlas production remained about
0.28 ms while its non-upsample composite was about 0.087 ms. Local lights had
zero samples, confirming that no local-light draw was submitted.

The directional pass therefore accounts for roughly 96% of the measured High
volumetric GPU time. High's directional shader costs about 6.3 times Normal;
atlas and composite are not meaningful optimization targets. Transitional log
intervals around quality changes reported intermediate directional averages
and must not be treated as stable modes.

The hidden non-persistent
`RenderVolumetricLightingSampleCountOverride` diagnostic permits a controlled
full-resolution comparison: High with override 16 versus High with override 32.
Zero restores automatic quality defaults. GPU timing accumulators and pending
queries reset whenever quality or effective sample count changes, and each log
line records both values, preventing mixed transition intervals from being
mislabelled.

The controlled High-resolution override capture measured approximately 4.84 ms
directional at 16 samples and 7.43 ms at 32 samples, excluding early outliers.
Doubling the maximum step count therefore adds about 2.59 ms (54%), while
full-resolution 16-step rendering already costs over three times the prior
half-resolution Normal directional pass. Both pixel coverage and step count are
material; sample count alone does not explain High's cost. Local-light draws
became active partway through the 32-step capture at roughly 0.23 ms, but their
separate query does not contaminate the directional measurement.

The subsequent half-resolution comparison measured a stable 1.26 ms at 16
samples and 1.82 ms at 32 samples. The second 16-sample budget therefore costs
about 0.56 ms at half resolution. A split-resolution High path—16 samples at
full resolution plus only the supplemental 16 samples at half resolution—has an
estimated 5.40 ms directional cost versus the measured 7.43 ms current High,
before the small cost of combining the two contributions. This predicts roughly
a 2 ms GPU saving while retaining all 32 depth samples.

### Depth-Aware Pattern Filter Diagnostic

`RenderVolumetricLightingBlurStrength` provides a non-persistent 0–1 A/B
control for a 3×3 box filter in the existing composite. All nine base weights
are equal so the coherent one-pixel lattice is suppressed more strongly than
by the earlier center-weighted 4/2/1 tent kernel. All eight neighbour weights
additionally use full-resolution relative depth, preventing scatter
from crossing strong silhouette discontinuities. The radius is one display
pixel in High and two display pixels in Normal because it follows the source
target texel size. Debug mode 1 applies the filter so the scattering texture
can be inspected directly; diagnostic modes 2 and 3 remain unfiltered. Timing
accumulators reset when blur strength changes, and log lines include the active
strength so composite cost can be compared without mixed intervals.

The original interleaved-gradient ray jitter remained visible after filtering
as coherent multi-pixel diagonal bands. The directional screen raymarch now
uses the alpha channel of the AS-owned 1024×1024 RGBA blue-noise asset
`skins/default/textures/as/as_blue_noise.png`. Exact texel fetching preserves
its distribution. This keeps the same `[0,1)` first-step offset and raymarch
workload, but gives the composite filter high-frequency noise without the
previous spatial lattice. The atlas jitter is unchanged.

The subsequently supplied `textures/as/bluenoise` dataset contains 1,733
multi-format assets: temporal sequences at 16² through 256², four 512²
variants, and one 1024² variant. The selected `as_blue_noise.png` is
byte-for-byte identical to that sole largest candidate,
`bluenoise/1024_1024/LDR_RGBA_0.png` (SHA-256
`54ad95664c4e751aa31d449291f1e7af0731fed996b76b663e2fe0a53e118e65`).
Smaller candidates would repeat more often on a 3440×2440 target, while HDR,
placement, and multi-frame encodings provide no advantage to the current
single scalar, spatially stable ray-jitter lookup.

The dataset and selected texture were generated with Moments in Graphics'
[`BlueNoise.py`](https://github.com/MomentsInGraphics/BlueNoise/blob/master/BlueNoise.py)
void-and-cluster blue-noise generator. This accounts for the standardized
dimension/format/frame naming and the byte-identical copies found through
different distribution sources.

Runtime validation of the initial blue-noise integration showed displaced tree
and terrain shadow silhouettes in the raw scattering target even with the
composite blur disabled. This must not be interpreted as evidence that blue
noise is unsuitable for first-sample raymarch jitter. The generator author's
[`Free blue noise textures`](https://momentsingraphics.de/BlueNoise.html#Jittering)
raymarch example explicitly prescribes one scalar uniform blue-noise value,
multiplied by the sample delta and added to the first ray sample; its published
32-sample foliage comparison reports substantially better quality than white
noise. The article also identifies uniform distribution, weak low-frequency
energy, isotropy, and tileability as the relevant properties.

The AS pre-optimization shader and current recurrence both use
`(sample_index + jitter) * step_length`, matching that prescription. The
observed ghosts therefore indicate an unresolved integration, texture-path, or
scene-response issue rather than a flaw inherent to whole-lattice blue-noise
jitter. Do not replace it with per-step independent jitter on the basis of this
capture alone.

For controlled follow-up, `RenderVolumetricLightingBlueNoiseStrength` provides
a non-persistent 0–1 blend between the ghost-free coherent jitter and fully
decorrelated blue noise. Zero avoids the blue-noise texture fetch in the
shader; intermediate values allow the strongest ghost-free decorrelation to
be identified without further builds. GPU timing logs include the blend and
reset their accumulation whenever it changes.

### MSM Beyond Hard Shadows Reference Comparison

The local `MSMBeyondHardShadowsCode` supplementary implementation and paper
were reviewed after blue-noise jitter exposed displaced shadow silhouettes.
`ComputeSingleScatteringRayMarchingDirectional()` in `ParticipatingMedia.fx`
uses equidistant whole-lattice jitter, not independent per-step jitter. Its
first evaluated position is `(1 - random_offset) / sample_count` along the ray;
complementing a uniform blue-noise scalar preserves its distribution and
spectrum, so this is equivalent in principle to the AS `(i + jitter) * step`
placement.

The important numerical difference is integration weight. The reference
computes each Beer–Lambert segment exactly as the difference of transmittances
at its two endpoints, then advances that weight using one precomputed
exponential decay. AS currently uses the left-point approximation
`exp(-density * sample_distance) * step_length`. The latter makes each binary
shadow result represent the complete interval with the attenuation at its
jitter-dependent point, increasing the visible effect of a changed jitter
pattern. Replacing it with exact discrete segment weights requires only the
same initial exponential/decay budget and is the strongest directly applicable
finding.

Other differences limit direct comparison: the supplied shader hard-codes 128
samples, uses one unfiltered shadow map rather than four cascades with five PCF
operations, and advances positions after transforming the full ray into a
single shadow projection. The paper reports acceptable moderate noise for 32
samples in a simpler scene but strong noise at 128 samples in a challenging
scene. Thus blue noise improves the error spectrum but cannot guarantee that
all low-sample, high-contrast shadow-volume structure disappears.

Runtime blending found `RenderVolumetricLightingBlueNoiseStrength = 0.3` to be
the initial sweet spot: displaced shadow silhouettes were no longer
distinguishable while some residual jitter remained. This is the controlled
baseline for exact-weight validation; it is not yet a shipping default.

The directional shader now implements exact Beer–Lambert segment integration
using `(T_start - T_end) / density`, retaining the existing outer density
multiplier and therefore the same physical/output normalization. A series
expansion handles near-zero optical step without cancellation, and zero density
uses the exact limiting segment length. One initial value is advanced by the
same precomputed decay recurrence; the predetermined far tail sums these exact
segment integrals with the existing geometric series. Ray positions, visibility
sampling, PCF operations, cascades, and sample counts are unchanged.

The validated `BlueNoiseStrength = 0.3`, `BlurStrength = 1.0` runtime capture
reported 45 FPS disabled, 41 FPS Normal, and 31 FPS High. Because Disabled was
45 rather than the controlled 55 baseline, these absolute FPS values are not a
valid before/after comparison. Their frame-time deltas are still informative:
Normal adds approximately 2.17 ms and High approximately 10.04 ms.

Asynchronous stage logs agree. Stable Normal intervals measured directional
roughly 1.43–1.49 ms, atlas 0.28–0.29 ms, local lights 0.069–0.071 ms, and
composite 0.25–0.28 ms. Stable High intervals measured directional roughly
8.67–8.81 ms, atlas 0.283–0.289 ms, local lights 0.245–0.259 ms, and composite
0.164–0.170 ms before a later 10.12 ms directional outlier. Thus the complete
pattern filter is inexpensive; directional shadow raymarching remains the High
quality bottleneck.

`RenderVolumetricLightingBlurRadius` is a non-persistent 1–2 source-texel
diagnostic. It widens the existing nine-tap depth-aware filter without adding
texture reads. Test 1.25, 1.5, and 2.0 with blur 1 and blue noise 0.3.

Review of `bluenoisefog.shader` found the same whole-lattice first-sample
jitter and mean-visibility integration already used here. Its additional lever
is temporal scrambling via `fract(blue_noise + frame * 0.61803398875)`. The
viewer has no active temporal reprojection/accumulation stage, so adopting this
would likely replace static residual structure with shimmer. It was not used.

Final visual tuning selected blur strength `1`, blur radius `2`, and blue-noise
strength `0`. The wider nine-tap filter best suppresses the interleaved lattice
without altering ray positions or producing displaced shadow silhouettes.
These are now the defaults; the non-persistent controls remain for diagnostics.

### Root Cause of Blue-Noise Ghosting

The displaced shadow silhouettes previously attributed to blue noise itself
(observed even at `BlueNoiseStrength = 1.0`, i.e. no blend with coherent
jitter) were traced to how the noise texture reached the GPU, not to the
jittering scheme. `sBlueNoiseImage` was loaded via `LLUI::getUIImage("ASBlue
Noise")`, the same generic UI-icon texture path used everywhere else in the
codebase for buttons, drag handles, and scroll-list icons
(`indra/llui/llbutton.cpp`, `lliconctrl.cpp`, etc.). That path resolves
through `LLViewerFetchedTexture`'s ordinary asset-streaming machinery:
progressive discard-level loading driven by on-screen draw size, and no
guarantee of an uncompressed internal format. A 1024x1024 texture never drawn
as a screen quad has no draw-size signal telling the streamer to fetch it at
full resolution, so it could sit at a reduced discard level indefinitely,
sampling a downsampled approximation of the source PNG. Either failure
(discard streaming or compression) destroys the exact per-texel byte values
the void-and-cluster generator produced, reintroducing spatial correlation and
producing displaced-looking shadow edges — this matches the observed symptom
far better than any property of blue noise as a jittering scheme.

The fix loads the texture directly through
`LLViewerTextureManager::getFetchedTextureFromFile()` instead of the UI-image
convenience wrapper, following the same pattern already used in
`llviewertexturelist.cpp` for `sFlatNormalImagep` (a normal map with the same
"must stay exact" requirement, loaded outside the discardable pipeline
specifically because a dataserver copy "has compression artifacts"):

- `MIPMAP_NO` and explicit `GL_RGBA`/`GL_RGBA` internal/primary format prevent
  mipmap generation and compressed-format upload.
- `LLGLTexture::BOOST_UI` matches the asset's registration in `textures.xml`
  and exempts it from ordinary discard-priority eviction.
- `setKnownDrawSize()` is called once with the texture's own full width and
  height immediately after fetch, since a texture sampled only via
  `texelFetch()` in a fullscreen pass has no natural on-screen draw size to
  report and would otherwise still compute a nonzero desired discard level.

This has not yet been validated at runtime. If it resolves the ghosting,
`RenderVolumetricLightingBlueNoiseStrength` should be revisited as a shipping
default rather than left at `0`, since the underlying decorrelation benefit
described in the Moments-in-Graphics article would no longer be blocked by a
texture-loading defect.

### Texture-Loading Fix Did Not Resolve Ghosting

Runtime testing after the texture-loading fix above still shows ghosting at
`RenderVolumetricLightingBlueNoiseStrength = 1.0`: a faint, duplicated copy of
real scene silhouette (leaf clusters and branches from the actual foreground
tree) appears offset to the right of the tree, overlapping the light shaft.
The same screen region at `BlueNoiseStrength = 0.0` (coherent interleaved
gradient noise only) shows no such duplication at all. This rules out the
texture-loading hypothesis (discard-level streaming or compressed internal
format corrupting the noise distribution) as the sole or primary cause — a
merely-degraded-quality noise texture would not reliably reproduce a coherent
duplicate of actual scene geometry. The fix should still be kept (it removes a
real defect: this texture must never be discard-streamed or compressed
regardless of its effect on this specific artifact), but it is not sufficient.

That the artifact is a duplicate of real scene content, not generic noise or
grain, points toward the jitter value affecting *which discrete sample or
shadow-map region* a ray reads, in a way that is spatially incoherent between
neighboring pixels only when blue noise (high pixel-to-pixel variance by
design) drives it — rather than a property of the noise texture's fidelity.
Two mechanisms were identified as plausible but unconfirmed and warrant
investigation with the viewer running:

1. **Far-tail early-exit branch instability**
   (`asVolumetricLightF.glsl`, the `sample_pos.z <= -shadow_clip.w` check
   inside the raymarch loop). Whether a given ray takes this branch at step
   `i` depends on `sample_pos.z`, which starts at `jitter * step_len` per
   pixel. Interleaved gradient noise varies smoothly pixel-to-pixel, so
   neighboring rays near this boundary transition through the branch
   similarly. Blue noise deliberately maximizes pixel-to-pixel variance, so
   two adjacent pixels near the boundary can resolve to structurally
   different code paths (full per-step loop vs. closed-form geometric-series
   tail) essentially independently — a candidate source of per-pixel
   incoherence at exactly the depth boundaries where shadow silhouettes sit.

2. **Blue-noise texture tiling aliasing against scene geometry**
   (`volumetricJitter()`'s `ivec2(screen_pos) % noise_size` wraparound,
   where `noise_size` is 1024x1024 and `screen_pos` is the volumetric
   target's own pixel coordinates, half display resolution in Normal mode).
   If the render target's pixel dimensions are not much larger than 1024 in
   the relevant axis, or the tree happens to repeat near a tile boundary,
   this could beat against scene structure. This does not obviously explain
   a duplicated *silhouette* rather than repeated *noise texture content*, so
   it is the weaker of the two hypotheses.

The most direct way to distinguish these (and rule out a third, unconsidered
mechanism) is a debug mode that visualizes the raw per-pixel jitter value
(before it is used to offset the first raymarch sample) as grayscale, so the
jitter field itself can be inspected directly against the ghost's screen
position — isolating "the noise input looks wrong" from "the noise input is
fine but the raymarch does something wrong with it." This has not been
implemented yet.

Current state: `RenderVolumetricLightingBlueNoiseStrength` remains at its
shipped default of `0` (coherent interleaved gradient noise only, no
ghosting). This is not a regression — it is the same conclusion the doc
already reached before the texture-loading investigation — but the ghosting's
actual root cause in the raymarch/jitter logic is still open.

`AS_VOLUMETRIC_PERFORMANCE_LOGGING` is now set to `0`. GPU stage timing and the
periodic log line are compiled out; the non-persistent debug settings
(`RenderVolumetricLightingSampleCountOverride`, blur strength/radius, blue-noise
strength) remain available since they do not depend on the logging macro.

## Future Work: Moment Shadow Maps

Review of Peters et al.'s "Beyond Hard Shadows" paper and its accompanying
`ParticipatingMediaUtility.fx`/`ParticipatingMedia.fx` reference implementation
(`.MSMBeyondHardShadowsCode/`) confirms the applicable lessons already adopted
here: whole-lattice single-scalar ray jitter (matching `ComputeSingleScattering
RayMarchingDirectional`'s `InitialLerpFactor` and `bluenoisefog.shader`'s
`startRayOffset`), and exact Beer-Lambert segment integration in place of a
left-point approximation. Rejecting `bluenoisefog.shader`'s temporal scrambling
(`fract(blue_noise + frame * goldenRatioConjugate)`) without an active temporal
reprojection stage is also consistent with the paper's own single-scattering
technique, which is likewise static per frame.

The paper's actual novel contribution — moment shadow maps (MSM) — is not yet
used here. Storing 4 (or 6) depth moments per shadow-map texel and prefiltering
them into transmittance-weighted row prefix sums lets single scattering along
each view ray be evaluated with **one shadow-map lookup**, replacing the
current per-step PCF raymarch entirely. The paper reports six-moment prefiltered
single scattering at 0.71–1.36 ms versus 1.48 ms for 32 ray-marched PCF
samples, at a cost that is independent of sample count (Section 2.6, Figure 8).
Applied here this would attack the directional pass directly, currently ~96%
of High's GPU cost.

This is a distinct architectural phase, not a tuning change, and would require:

- A rectified shadow-map coordinate system (Section 2.2) so view rays map to
  shadow-map rows with constant depth — either the paper's linear rectification
  or the non-linear resampling variant used for its final results.
- A new shadow-map format storing 4 or 6 moments with an optimized affine
  quantization transform (Section 1.2, 2.5), replacing today's `sampler2DShadow`
  cascades for the volumetric path specifically. The existing four-cascade PCF
  shadow maps used by surface shading would be unaffected.
- A prefix-sum generation pass per frame (compute shader, one thread per row)
  producing transmittance-weighted moments (Section 2.2, 2.6).
- Reconstruction (`EstimateIntegralFrom4Moments`/`6Moments`-equivalent) at each
  pixel from a single filtered sample, plus tuned biasing to control light
  leaking (the paper found biasing artifacts much less objectionable for
  scattering than for hard shadows).
- A decision on four versus six moments: four is faster but comparably weaker
  for the complex, whole-scene-spanning depth distributions typical of
  epipolar planes; six is the paper's recommended trade-off.

Because this replaces the shadow-sampling method rather than tuning existing
parameters, it does not meet the current pass's lossless/no-quality-regression
constraints as a drop-in change — it would need its own validation pass
(silhouette accuracy, light leaking at cascade-adjacent and epipole-adjacent
directions, and a fresh GPU-time comparison against the current five-tap PCF
sampler). Treat it as a separately approved phase, not an extension of the
current lossless optimization.
