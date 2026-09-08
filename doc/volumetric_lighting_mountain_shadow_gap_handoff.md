# Volumetric lighting: mountain shadow gap / halo-through-hill / rays gone (handoff)

## Status: UNRESOLVED, in-progress fix causes a regression. Needs fresh eyes.

## Original bug report

Sun/moon volumetric light shafts (god rays) visibly shine straight through
solid mountain/hill terrain (a heightfield region, not mesh/prim), converging
to a bright point exactly at the sun/moon's screen position, at low
sun/moon elevation (dusk/dawn or night with the moon near/behind a ridge).
Confirmed with `debug_mode 2` (raw occlusion, white=occluded/black=lit): a
narrow, sharp, near-zero-occlusion wedge sits right at/behind the mountain
silhouette, converging toward the light direction, while the mountain reads
correctly mid-gray (partially occluded) just off to the sides. Other
occluders (trees) shadow correctly in the same frame.

User confirmed this bug is a **regression**: it was not present before a
series of volumetric-lighting optimization commits.

## Root cause (high confidence, NOT yet fully fixed)

Commits `3961f8361f` ("Optimize volumetric raymarch sampling by distance")
and `513e3bb6b3` ("optimized normal quality...") reduce the raymarch's
shadow-test step count for rays ending on nearby geometry, down to a floor
of `min_steps = min(4, sample_count)`. See
`indra/newview/app_settings/shaders/class2/deferred/asVolumetricLightF.glsl`,
function `main()`, the `flat_ray_steps`/`steps` computation.

With only ~4 samples spread across the ray's length, a **thin occluder**
(a distant ridge crest, seen nearly edge-on from a low sun/moon) can fall
entirely between two consecutive point-sampled shadow tests: one step lands
just before the ridge, the next lands just after it, and neither sample
detects it, because each step tests shadow at a single point via
`asVolumetricDirectionalShadow(sample_pos, pos_screen)` (in
`asVolumetricShadowUtil.glsl`), not the segment the step actually covers.
This is a raymarch resolution problem, not fixable by tuning sample count
alone (an arbitrarily thin occluder can always fall between finite discrete
samples).

The existing `volumetricNearSilhouette()` edge-adaptive step multiplier
(added in `513e3bb6b3`, also in `asVolumetricLightF.glsl`) does not catch
this case: it only detects **depth discontinuities** between neighboring
screen pixels (for foliage/silhouette edges), not "this ray's shadow status
is about to flip" - a smoothly sloped, gently curving mountain ridge has no
depth discontinuity even though whether it occludes the sun changes sharply
as `ray_dir` sweeps past `light_dir`.

## Attempted fix (in progress, currently BROKEN - causes total regression)

Replaced the raymarch's point-sample shadow test with an **interval test**:
for each march step, project both the step's near AND far endpoint into the
shadow map's light space, and check whether the shadow map's raw STORED
depth (read directly, not through the `sampler2DShadow` hardware compare)
falls anywhere within/before that step's covered light-space depth range -
so a thin occluder within a step's span is caught regardless of exactly
where a point sample would have landed. This is exact down to the shadow
map's own texel resolution, independent of ray length or step count, if
implemented correctly. **It is currently NOT implemented correctly** - see
"Current broken state" below.

### GL constraint that had to be worked around

`GL_TEXTURE_COMPARE_MODE` is texture-OBJECT state, not per-binding-point
state. `pipeline.cpp`'s `allocateScreenBuffer()` sets
`GL_TEXTURE_COMPARE_MODE = GL_COMPARE_R_TO_TEXTURE` on the 4 sun shadow
render targets ONCE, for every future bind of that texture object on ANY
unit (see `indra/newview/pipeline.cpp` lines ~1203, ~1220 - grep
`GL_TEXTURE_COMPARE_MODE`). The existing `shadowMap0..3` (`sampler2DShadow`
in GLSL, hardware PCF compare) and a hypothetical raw-depth read of the SAME
texture object (`sampler2D`) cannot coexist in the same draw call - binding
the same GL texture both ways is undefined per spec, and toggling compare
mode per-draw would race every other consumer of these shared cascades
(surface shading, spot lights, etc., all sample the same 4 textures every
frame).

**Workaround implemented**: `ASVolumetricLighting::copySunShadowDepth()`
(new function, `indra/newview/asvolumetriclighting.cpp`) does a
`glBlitFramebuffer` depth-only copy of each of the 4 sun shadow cascades,
once per frame, into 4 new, separate, compare-mode-free
`LLRenderTarget`s owned by `ASVolumetricLighting`
(`sShadowDepthCopy[4]`, depth-only, `color_fmt=0`). These copies are bound
as new uniforms `shadowDepthMap0..3` (plain `sampler2D`) alongside the
existing `shadowMap0..3` (`sampler2DShadow`) bindings, via
`ASVolumetricLighting::bindShadowDepthMaps()`.

This part (the blit mechanism itself) currently reports NO GL error
(checked via an added `glGetError()` log after the blit, tag
`ASVolumetricLighting`, logged to `AyaneStorm.log`) and the FBO
completeness issue initially suspected (missing
`glReadBuffer(GL_NONE)`/`glDrawBuffer(GL_NONE)` on the depth-only blit FBOs)
has been fixed. **This mechanism is believed to be working**, though not
100% confirmed - see "What's actually been verified" below.

### Files touched (all AS-owned, no ownership tags needed; nothing upstream/SL-owned touched)

- `indra/newview/app_settings/shaders/class2/deferred/asVolumetricShadowUtil.glsl`
  - Added `uniform sampler2D shadowDepthMap0..3;` (raw depth, alongside
    existing `sampler2DShadow shadowMap0..3`).
  - Added `asVolumetricShadowFetchRange(sampler2D depth_map, vec3 stc_near,
    vec3 stc_far)`: the interval-occlusion test. **Logic here has already
    been revised once and may still be wrong** - see below.
  - Changed `asVolumetricDirectionalShadow()` signature from
    `(vec3 sample_pos, vec2 pos_screen)` to
    `(vec3 sample_pos, vec3 sample_far, vec2 pos_screen)` - now projects
    BOTH endpoints through the selected cascade's `shadow_matrix` and calls
    `asVolumetricShadowFetchRange` instead of the old point-sample
    `asVolumetricShadowFetch`.
  - The `#else` (multi-cascade cross-fade) branch, dead code under
    `AS_VOL_SINGLE_CASCADE=1`, was left using the OLD point-sample path
    unchanged (harmless since unreachable, but inconsistent/stale).

- `indra/newview/app_settings/shaders/class2/deferred/asVolumetricLightF.glsl`
  - Updated forward declaration and the one call site (in the main raymarch
    loop) to pass `sample_pos + sample_step` as the new `sample_far` arg.

- `indra/newview/app_settings/shaders/class2/deferred/asVolumetricAtlasF.glsl`
  - Same signature update; call site now passes `sample_far = ray_dir *
    segment_far` (this shader already segments the ray into 16 slices with
    known near/far bounds per slice, unlike the main raymarch's uniform
    steps).

- `indra/newview/asvolumetriclighting.h` / `.cpp`
  - New: `sShadowDepthCopy[4]` (`LLRenderTarget`, static members).
  - New: `copySunShadowDepth(LLPipeline&)` - the blit, called once per frame
    from `renderPass()` right before the raymarch/atlas shader binds.
  - New: `bindShadowDepthMaps(LLGLSLShader&)` - binds the 4 copies as
    `shadowDepthMap0..3` by uniform name (no `LLShaderMgr` reserved-uniform
    registry touched - uses the existing string-based
    `LLGLSLShader::bindTexture(const std::string&, LLRenderTarget*, bool
    depth, ...)` overload, same pattern as other ad-hoc uniforms already in
    this file, e.g. `as_active_light_dir`).
  - Both allocated lazily (sized to match each cascade's own resolution,
    which is independent of screen resolution) and released in
    `releaseResources()`.

## Regression: after this "fix", ALL god rays disappeared entirely, everywhere

User's exact words: "no more rays... NO MORE RAYS is me being upset because
there are no more god rays ANYWHERE". Not "shape changed" - the effect
produces essentially nothing, anywhere, any time of day.

## Diagnosis attempts so far (in order, each ruled out or inconclusive)

1. **First cut of `asVolumetricShadowFetchRange` had inverted logic**:
   originally tested `stored_depth` for being strictly INSIDE
   `[near_z, far_z]` (a narrow per-step window). This is wrong: it fails to
   keep a ray shadowed for the FULL remainder of its path once behind an
   occluder (it would flip back to "lit" as soon as the occluder's own
   depth no longer falls inside the CURRENT narrow step). **This was fixed**
   (changed to `stored_depth <= far_z`, matching the original hardware
   `GL_LEQUAL` compare-sampler's semantics of "occluded whenever ANY nearer
   surface exists, not just one inside a narrow window"). Rebuilding after
   this fix did NOT restore the rays - regression persisted.

2. **Suspected FBO incompleteness on the depth-only blit** (default
   `glReadBuffer`/`glDrawBuffer` = `GL_COLOR_ATTACHMENT0`, which doesn't
   exist on a depth-only FBO, could cause a silently-dropped blit on strict
   drivers). Added explicit `glReadBuffer(GL_NONE)`/`glDrawBuffer(GL_NONE)`
   on both blit FBOs, plus a `glGetError()` check with logging right after
   the blit. **Rebuilt and retested: no GL error logged, regression
   persisted.** This theory is likely ruled out (or the error simply isn't
   being triggered even though something else is still wrong).

3. **User's own theory** (his words): "the halo was that the bug is not
   fixed but rays were no longer shown because no shadow were producing
   rays" - i.e., the halo-through-hill and the missing-rays might be the
   SAME underlying failure (volumetric occlusion computation broken/
   producing near-zero visibility contribution everywhere), not two
   separate bugs. This is plausible and NOT yet disproven.

4. **Checked `debug_mode 2` (raw occlusion) again on the "fixed" build**:
   user reports it is NOT uniformly 100% black - zooming the camera WAY out
   (far enough that land is no longer visible) eventually shows some white
   (occluded) appearing. At any normal/realistic viewing distance/zoom, it
   reads as 100% black (zero occlusion, i.e. "fully lit") EVERYWHERE. This
   is the most concrete diagnostic data point available and is NOT yet
   explained. Two live theories, neither confirmed:
   - (a) The occlusion test only ever returns "occluded" for extremely
     large distances/depths (a units/scale bug making the effective
     occlusion threshold way too large for normal scene scale), OR
   - (b) `shadowDepthMap0..3` are not actually receiving valid copied
     depth data (reading back some default/cleared/undefined value that
     happens to only cross the comparison threshold at extreme synthetic
     depth), which would point back to the blit/copy mechanism after all,
     despite no GL error being reported.

5. **Verified NOT the cause** (checked and ruled out with actual code
   reading, not guessing):
   - Shadow-matrix depth convention: confirmed `mSunShadowMatrix` already
     bakes in the `[-1,1] -> [0,1]` NDC-to-depth-texture remap on the C++
     side (`pipeline.cpp` ~line 11727-11741, the `trans` matrix multiplied
     into `mSunShadowMatrix[j]`), so `shadow_matrix * pos` in the shader is
     already in the same `[0,1]` convention as the stored depth texture.
     Not a units mismatch.
   - Cascade-early-out / far-shadow-distance logic
     (`spos.z <= -shadow_clip.w -> return 1.0`): unchanged from before this
     work, was already correct/working, not implicated.
   - Frame ordering: confirmed `generateSunShadow()` (writes this frame's 4
     cascades) runs early in `display()`
     (`indra/newview/llviewerdisplay.cpp` ~line 951), well before
     `ASVolumetricLighting::renderPass()` (called from
     `pipeline.cpp:10099`, inside `renderDeferredLighting()`) - so
     `copySunShadowDepth()` is never copying stale/previous-frame data.
   - Shader cache staleness: **explicitly ruled out per AGENTS.md line 49**
     - "The build script DELETES the shader cache... A stale cache is NEVER
       the cause of issues." Do not investigate this again. Do not
       bump `ASVolumetricLighting::shaderCacheRevision()` - this is
       forbidden per AGENTS.md ("No bumping shader version during
       development. It forces a build that triggers Link Time Optimization
       which takes a lot of time.") and the user has confirmed this rule is
       absolute.

## What's actually been verified vs. still unknown

**Verified working / ruled out**: shader compiles and links (no GLSL
errors in `AyaneStorm.log`, `shader_level: i5` present); frame ordering;
depth-space unit convention; blit reports no GL error after the
read/draw-buffer fix.

**NOT yet verified**: whether `shadowDepthMap0..3` uniforms are actually
found by the linked shader program (i.e., whether
`LLGLSLShader::bindTexture()`'s internal `getUniformLocation()` call
returns a valid channel, not -1) and whether the texture that's ACTUALLY
bound in-shader is the freshly-blitted copy with real geometry depth in it,
as opposed to an all-default/cleared texture. A diagnostic log line was
added for this (tag `ASVolumetricLighting`, logs the bind channel for each
`shadowDepthMap0..3` on both `gASVolumetricLightProgram` and
`gASVolumetricAtlasProgram`, once, on first call) but **the user has not
yet rebuilt/retested with this specific diagnostic in place** - this is
the very next concrete step and its output was never captured before the
user asked to hand off.

## Recommended next steps for whoever picks this up

1. Rebuild with the diagnostic log already in place (see
   `bindShadowDepthMaps()` in `asvolumetriclighting.cpp` - it's already
   written, just needs a build+run+log-check) and read the
   `ASVolumetricLighting` tagged lines in `AyaneStorm.log`. If channel is
   -1 for any `shadowDepthMapN`, the uniform isn't reaching the shader -
   check for a name typo, or whether the GLSL compiler/linker is stripping
   it as unused (possible if some code path never actually samples it, or
   if `#if AS_VOL_SINGLE_CASCADE` dead-code elimination removed a branch
   whose reference kept it live).
2. If the uniform IS bound correctly, add a raw debug-visualization mode
   that outputs `texture(shadowDepthMap0, screen_uv).r` directly (bypass
   all the projection math) to confirm the copied texture actually contains
   real, varying depth data and not a uniform default value - this
   isolates "the copy has no data" from "the copy has data but the
   comparison math is wrong."
3. If the copy has real data but comparison is still wrong: re-derive
   `asVolumetricShadowFetchRange()` from scratch against a KNOWN-correct
   single point test first (i.e., verify `asVolumetricShadowFetchRange`
   with `stc_near == stc_far` reproduces the EXACT same result as the old
   `asVolumetricShadowFetch` point-sample, for a variety of camera angles/
   times of day) before re-introducing the near/far span logic. This
   isolates "the raw depth comparison direction/bias is wrong" from "the
   span logic is wrong."
4. Given how many wrong turns this took via static reasoning alone,
   consider a smaller, independently-testable increment: implement ONLY
   the single-point raw-depth comparison first (mathematically equivalent
   to the existing hardware-compare path, just via manual comparison
   instead of `sampler2DShadow`), confirm rays look IDENTICAL to before any
   of this work started, and only then extend to the near/far interval.
   Currently the interval logic and the raw-depth-instead-of-hardware-
   compare change were both introduced at once, making it hard to isolate
   which one broke things.
5. Do not re-litigate: shader cache staleness (forbidden/ruled out, see
   above), the `[-1,1]->[0,1]` depth convention (confirmed correct), or
   frame/blit ordering (confirmed correct).

## Working tree state

All changes are uncommitted. The previous interval-depth attempt is staged;
the fresh-review replacement below is an unstaged working-tree correction on
top of it, preserving the staged attempt for direct comparison. No destructive
git operations have been run. The user has NOT asked for a commit.

## Fresh review and replacement fix (2026-09-09)

The interval-depth approach above was rejected after re-derivation. A single
raw shadow-depth fetch at the projected segment midpoint is not an exact
segment/terrain intersection test: the segment's projected XY can cross more
than one shadow texel, and one stored depth describes only the nearest caster
on one light-space ray. Comparing that value only with the segment's far Z also
changes the established point-sample/PCF estimator. The approach added four
depth copies, raw FBO state, custom sampler bindings, and a second behavior
change at once, then removed all visible rays in runtime testing. A missing
uniform log in the available runtime log does not rescue the underlying
algorithm.

The repository history provides a narrower causal boundary. Commit
`3961f8361f` introduced endpoint-distance scaling with a four-sample floor;
the reported terrain gap did not exist before the optimization series. Later
quality work restored configured flat counts of 16/32/64 but retained that
distance scaling, so ordinary geometry-ending rays could still be reduced to
four samples. The clean reference configuration documented in
`volumetric_lighting_sample_count_question.md` used the full configured count.

The replacement fix therefore:

- restores `asVolumetricDirectionalShadow()` to the established
  `sampler2DShadow` hardware-compare path;
- removes the four raw shadow-depth copies, blit FBOs, and custom sampler
  bindings;
- restores the atlas call to its original point shadow sample; and
- uses `flat_steps` for every ray, with the existing silhouette multiplier
  still applied, instead of reducing ordinary rays by endpoint distance.

This is deliberately a correctness-first regression fix. Normal/High now use
16 samples for flat texels and 32 at detected silhouettes; Very High and Ultra
use 32 and 64 respectively. It costs more than the distance-scaled path for
near geometry but returns to the sampling density already validated during the
quality work and removes the separate all-rays regression. No shader revision
was bumped and no build was attempted, per repository instructions.

Runtime acceptance test: reproduce the same low-elevation sun/moon behind the
same terrain in debug mode 2 and normal mode 0. The narrow black/unoccluded
wedge must be absent while ordinary rays remain visible. Also check a nearby
tree/structure and one transparent atlas consumer to catch directional or
atlas regressions. If the gap survives at full configured counts, the stated
sample-count root cause is falsified; capture mode 2 at override 64 before
changing shadow-map algorithms again.

## Confirmed below-horizon terrain-culling cause (2026-09-09)

The PCF/cascade/full-step build reproduced the same defect. Those
de-optimizations were therefore removed; they did not affect the bug and should
not impose their GPU cost.

The user's observation that the defect occurs only below zero light elevation
led to the actual terrain-specific mechanism. `LLPipeline::renderShadow()` and
`renderGeomShadow()` enable back-face culling. Second Life terrain is a
single-sided heightfield made of upward-facing triangles. Once the active sun
or moon direction drops below zero, the shadow camera views that heightfield
from underneath, making every terrain triangle a back face; terrain disappears
from the shadow map. Closed objects such as trees still expose other faces and
continue casting, exactly matching the reported contrast between mountains and
trees. The volumetric twilight code deliberately remains active below zero, so
it then interprets the missing terrain caster as unobstructed light and draws
the converging wedge.

`LLDrawPoolTerrain::renderShadow()` now disables `GL_CULL_FACE` only around the
terrain draw and only when the currently selected twilight source direction has
negative Z. The scoped GL state restores culling immediately afterward. At and
above zero elevation the existing path and performance are unchanged. The edit
is enclosed in `<AS:Chanayane>` ownership tags and retains the original
`drawLoop()` as a comment, as required for an `ll*.cpp` file.

Runtime acceptance test: use the same scene and move the selected sun/moon
across zero elevation. The terrain shadow and absence of the bright wedge must
remain continuous across zero; tree and ordinary object shadows must remain
unchanged. Also check from above terrain while the source is below zero to
ensure the two-sided shadow draw does not introduce unexpected self-shadowing.

### Correction after positive-elevation reproduction

The user reproduced the terrain-only leak at approximately +6 degrees. Zero
elevation is therefore not the boundary. The same mechanism applies to steep
terrain slopes at low positive angles: triangles whose upward normal faces
away from the light are back-facing to the shadow camera and are culled, even
though the single-layer terrain must physically remain an occluder. The fix was
corrected to disable culling for every terrain shadow draw, independent of
elevation. This is still terrain-only and scoped to the shadow pass; meshes and
ordinary scene rendering are unchanged. Because the heightfield has only one
geometric layer, this does not submit or shade a duplicate face—it merely lets
the existing triangle write shadow depth from either side.

## First replacement-fix runtime result and second correction

The user rebuilt and supplied
`AyaneStormOS-Normal_Cjyu9BCemp.jpg`: rays returned, proving that removal of the
raw-depth/copy path fixed that path's total-disappearance regression. The
original narrow bright triangular volume through/along the right-hand terrain
silhouette remained. Full configured sample counts therefore did not solve the
original defect, and the earlier claim that the four-sample floor was its root
cause is falsified.

The remaining directly relevant regression boundary is commit `e5dcc8194a`.
It changed two parts of directional shadow reconstruction together: the
five-comparison PCF footprint became one hardware-bilinear comparison, and
four-cascade overlap blending became one hard-selected cascade. The screenshot
matches lost shadow coverage at a terrain/light silhouette more closely than a
missed view-ray interval. To return to a genuinely known-good shadow estimator,
both optimizations are now reverted as a unit in
`asVolumetricShadowUtil.glsl`: the original five-comparison PCF function and
overlapping cascade weights are restored. Fixed full raymarch counts remain in
place, so the resulting test is correctness-first and intentionally equivalent
to the pre-optimization sampling/shadow structure rather than another new
shadow algorithm.

Next runtime test: reproduce the identical camera, environment, and terrain in
normal mode 0. If the triangular leak is gone, test camera motion and one
ordinary tree shadow for stability. If it remains, capture the same view in
debug mode 2; at that point the optimization-series explanation is falsified
more broadly and the next investigation must inspect terrain inclusion and
rasterization in the actual shadow cascade, not add more raymarch samples.

## Final result

**BOKT (2026-09-09).** The user confirmed that unconditional two-sided terrain
shadow casting fixes the mountain light leak, including the low positive-angle
case. The cause was back-face culling of the single-sided terrain heightfield,
not raymarch sample density, PCF footprint, cascade selection, or shader-depth
copying. The sampling de-optimizations and raw-depth-copy experiment were
removed; the final effective source change is confined to
`LLDrawPoolTerrain::renderShadow()`.
