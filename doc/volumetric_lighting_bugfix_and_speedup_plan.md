# Volumetric Lighting: Bug Fixes and Speed-Up Plan

Author: chanayane@firestorm. Review date: 2026-09-02.
Scope: `asvolumetriclighting.cpp/.h`, `asVolumetricLightF.glsl`,
`asVolumetricShadowUtil.glsl`, `asVolumetricAtlasF.glsl`,
`asVolumetricCompositeF.glsl`, `asVolumetricLocalLightF.glsl`, and their
callers in `pipeline.cpp`, `lldrawpoolalpha.cpp`, `lldrawpoolsimple.cpp`,
`lldrawpoolwater.cpp`. Companion to
`volumetric_lighting_performance_optimization.md` (measurements live there).

This file is an implementation brief for another agent. Every item states
what to change, where, why, and what to verify. Do not bump the shader
version or `shaderCacheRevision()` while iterating. The user builds.

## 0. Instructions for the Implementing Agent (read first, every session)

You are executing this plan; you did not write it. Follow it literally.
Where the plan and the code disagree, stop and report; do not improvise.

Ground rules (from `AGENTS.md`, restated because they are easy to break):

- Read-only git only. Never `git add/commit/checkout/stash/mv/rm`.
- Do not build. Do not ask for a rebuild per small change; batch edits as
  section 7 says.
- Do not bump the shader version or `shaderCacheRevision()`.
- A stale shader cache is never the cause of a problem. Do not suggest it.
- Do not edit `ll*` or `fs*` files for this plan. None are needed.
- Files under `as*` are AS-owned: no ownership tags, but keep comments.
- Do not spawn agents.

Traps specific to this work:

1. **Texture channels.** Never call `shader.bindTexture("name", ...)` or
   `unbindTexture("name", ...)` for a sampler that is not in
   `LLShaderMgr::mReservedUniforms`. It indexes the wrong table (section
   2.1). Use `getUniformLocation` + `mActiveTextureChannels` +
   `glUniform1i` + `gGL.getTexUnit(channel)->bind(...)`, exactly like
   `renderTransparencyAtlas()` does for `previous_slice_integral`.
2. **Uniform names are hashed.** Use `LLStaticHashedString` for every
   uniform set/get; a plain `std::string` overload may not exist or may be
   slow. Copy the pattern already in the file.
3. **Do not remove uniforms that other attached shader files use.**
   `asVolumetricShadowUtil.glsl` is linked into the light and atlas
   programs; it uses `shadow_matrix`, `shadow_clip`, `shadow_bias`,
   `shadow_res`, `shadowMap0..3`. Keep those. Only remove the ten
   uniforms listed in 2.4 and the two blue-noise uniforms.
4. **`SUN_SHADOW` define.** The shadow util body is inside
   `#if defined(SUN_SHADOW)`; the program gets that define from
   `mFeatures.hasShadows = true`. Do not remove the feature flag or the
   guard, or shadow maps stop being bound and rays disappear.
5. **View-space z is negative forward.** Cascade i covers
   `z > -shadow_clip[i]`; "farther" means "more negative". Every `<` in the
   reference sampler code is intentional.
6. **Orthographic w.** Sun cascades are orthographic; `stc.w == 1`. Keep the
   divide anyway (cheap, safe).
7. **Composite runs at display resolution; the source may be half.**
   `emissiveRectDelta` is `1/sourceSize`, not `1/screenSize`. The gather
   code is written in source-texel units; do not "fix" it to screen units.
8. **`getDepth()` returns device depth, not distance.** Linearize with
   `linearDepth(d, zNear, zFar)`; upload `zNear/zFar` from
   `LLViewerCamera::getInstance()->getNear()/getFar()`. Do not compare raw
   device depths; they are packed near 1.0.
9. **Jitter must use `gl_FragCoord.xy`** (integer target pixels), never
   `vary_fragcoord` (0..1). Bayer needs integer pixel coordinates.
10. **Local lights render into the same target** with additive blending
    after the directional pass. If you change the target format, the blend
    must still work (float formats do; do not pick an integer format).
11. **Atlas alpha is transmittance and is consumed by alpha/water/simple
    shaders.** Do not change the atlas format, tile layout, slice count, or
    its alpha math.
12. **Debug modes.** Modes 2/3 must stay unfiltered raw values; mode 1 gets
    the gather; modes 10/11 read the atlas. Do not route them through the
    new filter accidentally.
13. **Settings removal.** When removing a setting from `settings.xml`, also
    remove every `LLCachedControl` / `gSavedSettings` reference and any
    XUI control bound to it, else the viewer logs errors or asserts at
    startup. Grep the whole `indra/newview` tree for the name.
14. **Do not delete files with git.** List files to delete for the user.
15. **Do not report a speed-up you did not measure.** All numbers in this
    plan are estimates. Turn on `AS_VOLUMETRIC_PERFORMANCE_LOGGING`, let
    the user capture, then report the logged averages. Set it back to 0
    before finishing.
16. **Do not "improve" the physics.** Segment integration, far-tail
    geometric series, brightness scale, phase function, and altitude fade
    are validated. Leave them.

Report format at the end: list each section number touched, the files
changed, anything skipped and why, and the exact visual checks that still
require the user's eyes.

## 1. Honest Assessment

- The directional raymarch is ~96% of High's GPU cost (measured 9.7 ms at
  3440x1440 High, 1.5 ms Normal). Cost = pixels x steps x shadow fetches.
  Today one step does 5 shadow fetches, and up to 10 inside cascade
  cross-fade bands (which cover ~40% of each cascade's range).
- "Close to no FPS impact" at full resolution with 32 steps and 5-tap PCF
  is NOT achievable on this hardware. It IS achievable by removing
  redundant work that the existing jitter+filter already hides:
  1-tap PCF (section 4.1), interleaved 4x4 sampling with fewer steps per
  pixel (4.3), single-cascade selection (4.2). Realistic target after
  4.1-4.3: High directional ~0.7-1.2 ms, Normal ~0.25 ms. High total
  ~1.2-1.5 ms including atlas+composite, i.e. roughly 55 -> 51 FPS instead
  of 55 -> 34 in the controlled scene. These are estimates; measure.
- The previous "lossless" pass proved arithmetic-only rewrites give nothing:
  the shader is texture-fetch bound. Do not spend more effort there.
- The blue-noise ghosting is a real code bug (section 2.1), not a property
  of blue noise. After the fix, blue noise will still be a worse choice
  than a stratified pattern for the existing small reconstruction filter
  (section 3). Recommendation: fix the bug so the diagnosis is confirmed,
  then replace both IGN and blue noise with a Bayer 4x4 interleaved pattern
  and delete the blue-noise texture path.
- Temporal accumulation (section 6) is the only route beyond that. The
  viewer has no previous-frame view/projection matrices or history targets
  anywhere in `pipeline.cpp`; it would be a new subsystem with real
  ghosting risk. Not recommended for the implementing agent now.

## 1a. Expected Outcome (estimates, to be replaced by measurements)

Controlled scene, 3440x1440, baseline 55 FPS with the feature disabled.
Extrapolated from the measured per-stage GPU timings in the previous doc;
not yet measured.

| Metric | Today | After Phases A-C |
|---|---:|---:|
| High: feature GPU cost | ~10.2 ms | ~1.2-1.5 ms |
| High: FPS | 34 (-38%) | ~50-51 (-8%) |
| Normal: feature GPU cost | ~2.2 ms | ~0.5-0.7 ms |
| Normal: FPS | 49 (-11%) | ~53 (-4%) |
| VRAM, High / Normal | 59 / 29 MB | ~35 / ~20 MB; 0 when disabled |
| Install size | | -8.4 MB |

Contributions to the High saving, roughly: 1-tap fetch ~65%, fewer steps
with interleaved sampling ~65% of the remainder, single cascade ~20% of
the remainder, composite/CPU cleanups <0.2 ms. The ~1 ms floor is the
cost of full-resolution ray setup, 8-12 fetches per pixel and the 16-tap
gather; going below it requires temporal accumulation (section 6).

## 1b. Visual Quality Contract (read before implementing)

Rule: output must be equal or better than today. A change may only lower
quality if the gain is large and the loss is subtle. Each item below is
classified; anything marked "trade" must be A/B'd and reported, not
silently shipped.

| Item | Quality effect | Class |
|---|---|---|
| 2.1 blue-noise binding fix | Removes a ghost artifact. | Improvement |
| 2.3, 2.4, 2.6 CPU/uniform cleanups | Bit-identical output. | Lossless |
| 2.5 linear depth in composite | Same depth value via a cheaper formula; drop of the normal guide removes false rejection of scatter on curved continuous surfaces. | Lossless / slight improvement |
| Section 3 Bayer 4x4 + 4x4 gather | Replaces the IGN lattice (which needed a blur to hide) with exact stratification. Fewer bands, no ghosts, sharper silhouettes because the gather is depth-aware at 16 taps instead of 9. | Improvement |
| 4.1 one hardware-bilinear fetch instead of 5 taps | Per-step shadow edge is slightly less soft (2x2 vs ~4x4 texel footprint). Because every pixel integrates 8-32 steps and the 4x4 gather averages 16 pixels, the softening the 5-tap provided is reproduced by the integration and filter. Expected to be indistinguishable in stills and motion; contact shadows inside thin shafts are the place to look. | Trade, expected unnoticeable; verify |
| 4.2 single-cascade selection | Cross-fade is a surface-shading device. Inside the integral, a hard split changes shadow-map texel size at one depth; jitter and the gather blend it. Possible faint band at a split with very fine occluders (foliage). Ship only behind the `#define` and only if no band is visible at all four splits in motion. | Trade, must verify; revert if visible |
| 4.3 fewer steps per pixel (8/12) with interleaved sampling | Effective samples per 4x4 block rise from 16 (today, with IGN's approximate 3x3 stratification) to 128-192 with exact stratification. Per-pixel raw noise before the gather is higher; after the gather it is lower than today. If 12 shows any banding in High, use 16 (still 2x cheaper than 32 and, after the gather, better than today's 32). | Improvement after filter; verify count |
| 4.4 shadow-space recurrence | Float accumulation error over 32 additions is far below shadow texel size. | Lossless |
| `R11F_G11F_B10F` target | 10-11 bit mantissa vs 16F. Scatter is added to an HDR screen and then tonemapped; risk is banding in very dim moonlight scatter. Test moonlit scene at max density before adopting. | Trade, small; optional |

Net expectation after the whole plan: equal or better image at High and
Normal, with the honest caveat that 4.1 and 4.2 are reductions in raw
per-step filtering that are expected, not proven, to be hidden by the
integral and the new filter. The implementing agent must capture
before/after stills (debug mode 1 and final image) at the roofline shaft,
dense foliage shadow, and each cascade split, and report any visible
difference rather than assume none.

## 2. Bugs (fix all)

### 2.1 CRITICAL: `blueNoiseMap` samples texture unit 0 (the G-buffer), not the noise

Root cause, verified in `llrender/llglslshader.cpp`:

- `LLGLSLShader::mapUniform()` only assigns a texture channel
  (`mTexture[i] = mapUniformTextureChannel(...)`) for uniforms in
  `LLShaderMgr::mReservedUniforms` or the per-shader `uniforms` list. A
  sampler named `blueNoiseMap` is neither, so its GLSL sampler keeps the
  default value 0 = texture unit 0.
- `bindTexture(const std::string&, LLTexture*)` does
  `mTexture[getUniformLocation(name)]`: it indexes the reserved-uniform
  channel table with an unrelated GL location. It either returns -1 or binds
  the noise texture onto some other reserved sampler's unit (possibly a
  shadow cascade or the depth map). `unbindTexture("blueNoiseMap")` has the
  same defect.
- In `bindDeferredShader()` unit 0 is `DEFERRED_DIFFUSE` = full-resolution
  `deferredScreen` attachment 0. `texelFetch(blueNoiseMap, ivec2(gl_FragCoord))`
  therefore reads the albedo G-buffer's alpha at the volumetric target's own
  pixel coordinates. In Normal (half-res target) this is a 2x-magnified copy
  of scene content anchored at the bottom-left, which is exactly the
  observed "duplicate of the foreground tree, displaced to the right".
  In High it coincides with the scene silhouette and looks like displaced
  shadow edges. `RenderVolumetricLightingBlueNoiseStrength = 0` skips the
  fetch, hence no ghost.

Fix (in `renderPass()`, replacing the `bindTexture("blueNoiseMap")` /
`unbindTexture("blueNoiseMap")` pair). Use the same appended-channel
pattern `renderTransparencyAtlas()` already uses for `previous_slice_integral`:

```cpp
static const LLStaticHashedString blue_noise_sampler("blueNoiseMap");
S32 blue_noise_channel = -1;
if (sBlueNoiseImage.notNull() && sBlueNoiseImage->hasGLTexture())
{
    const S32 location = gASVolumetricLightProgram.getUniformLocation(blue_noise_sampler);
    const S32 channel = gASVolumetricLightProgram.mActiveTextureChannels;
    if (location > -1 && channel >= 0 && channel < gGLManager.mNumTextureImageUnits)
    {
        glUniform1i(location, channel);
        gGL.getTexUnit(channel)->bind(sBlueNoiseImage.get());
        gGL.getTexUnit(channel)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_WRAP);
        blue_noise_channel = channel;
    }
}
// ... draw ...
if (blue_noise_channel > -1)
{
    gGL.getTexUnit(blue_noise_channel)->unbind(LLTexUnit::TT_TEXTURE);
}
```

Only set `blueNoiseStrength` > 0 when `blue_noise_channel > -1`; otherwise
force 0 so an unloaded texture never yields a constant jitter.

Verify: `RenderVolumetricLightingBlueNoiseStrength = 1`, debug mode 2 or 3,
Normal quality, foreground tree against sky. The magnified silhouette must
be gone. Then implement section 3 and remove the blue-noise path entirely
(see 2.2).

### 2.2 Duplicate 4.2 MB texture in the skin

`skins/default/textures/as_blue_noise.png` and
`skins/default/textures/as/as_blue_noise.png` are byte-identical.
`textures.xml` and the code reference only the `as/` copy. Delete the
top-level copy (user runs the git removal; agents must not use mutating git
commands). If section 3 is adopted, delete both, the `ASBlueNoise` entry in
`textures.xml`, `sBlueNoiseImage`, the `blueNoiseMap`/`blueNoiseStrength`
uniforms, and the `RenderVolumetricLightingBlueNoiseStrength` setting.

### 2.3 Per-draw CPU work in `bindTransparencyAtlas()`

Called from `lldrawpoolalpha.cpp` (3 sites), `lldrawpoolsimple.cpp` (4),
`lldrawpoolwater.cpp` (1), i.e. many times per frame. Each call does:
`isEnabled()` (a string-keyed `gSavedSettings.getBOOL`), `getDebugMode()`,
`LLWorld::resolveLandHeightAgent()`, `getScatterAsymmetry(isVolumetricSunSource())`
(sky settings lookup), and five uniform uploads.

Fix: compute once per frame in `renderPass()` into private statics
(`sFrameAtlasEnabled`, `sFrameSceneDensity`, `sFrameAlbedo`,
`sFrameAsymmetry`, `sFrameDensity`). `bindTransparencyAtlas()` only binds
the texture and uploads those cached values. Make `isEnabled()` use
`LLCachedControl<bool>` for `RenderVolumetricLighting` (it is also called
from `pipeline.cpp` several times per frame).

### 2.4 Dead uniforms and redundant settings reads

`asVolumetricLightF.glsl` and `asVolumetricAtlasF.glsl` declare
`sun_dir, moon_dir, sun_up_factor, sunlight_color, moonlight_color,
moon_horizon_tint, moon_horizon_tint_strength, moon_horizon_elevation,
moon_horizon_tint_height, moon_phase_illumination` but never use them
(`asVolumetricShadowUtil.glsl` does not use them either). The linker
removes them; `applyMoonAppearance()` then does five `gSavedSettings`
string lookups plus five no-op uniform sets, twice per frame.

Fix: delete those declarations from both shaders. Delete
`applyMoonAppearance()` and both call sites. Keep the tint math inside
`applyDirectionalInvariants()` but read `ASMoonHorizonTint*` via
`LLCachedControl`. Drop the `SUN_UP_FACTOR` uniform sets for these two
programs (nothing consumes it once `sun_up_factor` is gone).

### 2.5 Composite depth taps use a full matrix multiply

`depthSimilarity()` calls `getPosition(uv).z` (mat4 x vec4 + divide) per
tap: 4 upsample taps + 8 blur taps + center = 13 per display pixel in
Normal, 9 in High. Replace with
`linearDepth(getDepth(uv), zNear, zFar)` from `deferredUtil.glsl`.
`zNear`/`zFar` are NOT standard deferred uniforms: declare them in the
composite shader and upload `LLViewerCamera::getInstance()->getNear()` /
`getFar()` from `draw_composite` (same pattern as
`screenSpaceReflPostF.glsl`; grep `"zNear"` in `pipeline.cpp` for the
upload). Also drop `normalSimilarity()` and the `NORMAL_MAP` bind: scatter is
low-frequency and the depth guide is sufficient (previous doc, item 3).
Expected composite saving ~0.1 ms; mainly matters after section 4.3
increases tap count.

### 2.6 Minor / no action

- Far-tail early exit (`ray_dir.z < 0.0 && sample_pos.z <= -shadow_clip.w`)
  matches the sampler's `spos.z <= -shadow_clip.w -> 1.0` contract. Correct;
  it was wrongly suspected as a ghost cause. Keep.
- Atlas jitter uses `screen_uv * vec2(4096, 2160)`; harmless. When section 3
  lands, switch it to the same Bayer lookup on `gl_FragCoord.xy` for
  consistency.
- `sVolumetricTarget` is `GL_RGBA16F`; its alpha is never consumed
  (composite derives transmittance itself). `GL_R11F_G11F_B10F` halves
  target bandwidth and is blendable (local lights use additive blend). Low
  priority, easy: change the two `allocate()` calls. Verify no visible
  banding in dim moonlight scatter.

## 3. Jitter: Replace IGN + Blue Noise With Bayer 4x4 Interleaved Sampling

Why the current design fights itself: with N whole-lattice samples per
pixel, each pixel's error is a deterministic function of its jitter `j`.
The composite's 3x3 depth-aware box only cancels that error if the 9
neighbours' `j` values are stratified over [0,1). IGN approximately is
(that is what it was designed for), blue noise is not at a 3x3 scale; hence
residual grain in silhouette-shaped regions even with a correct texture.
Blue noise pays off only with a large kernel or temporal accumulation.

Interleaved sampling (Keller/Heidrich; Toth/Umenhoffer real-time volumetric
lighting; used by CryEngine): a 4x4 Bayer matrix gives 16 exactly stratified
offsets, and a 4x4 gather over the block reconstructs 16 x N uniformly
spaced samples. This lets the per-pixel step count drop ~2-4x with equal or
better quality, and produces no lattice bands and no silhouette ghosts.

Shader (`asVolumetricLightF.glsl`), replacing `interleavedGradientNoise`,
`volumetricJitter`, `blueNoiseMap`, `blueNoiseStrength`:

```glsl
float volumetricJitter(vec2 screen_pos)
{
    const float bayer[16] = float[16](
         0.0,  8.0,  2.0, 10.0,
        12.0,  4.0, 14.0,  6.0,
         3.0, 11.0,  1.0,  9.0,
        15.0,  7.0, 13.0,  5.0);
    ivec2 p = ivec2(screen_pos) & 3;
    return (bayer[p.y * 4 + p.x] + 0.5) / 16.0;
}
```

Composite (`asVolumetricCompositeF.glsl`): replace the 2x2 bilateral
upsample + 3x3 blur with ONE depth-aware gather over the 4x4 source-texel
window `floor(source_position) - 1 .. + 2` (16 scatter taps, 16 depth taps
using 2.5's cheap linear depth). Weight = bilinear tent over the window
(or plain box) x `depthSimilarity`. Keep the existing bilinear fallback
when the weight sum is ~0. This single path serves both Normal (source =
half res) and High (source = full res); `depthAwareUpsample` and
`scatterBlurStrength/Radius` become unnecessary. Remove the
`RenderVolumetricLightingBlurStrength/BlurRadius` settings.

Reference implementation (replaces everything in `main()` after the
`showAlphaChannel` branch; `linearViewDepth(uv)` is
`linearDepth(getDepth(uv), zNear, zFar)` from 2.5):

```glsl
float depthWeight(float tap_depth, float center_depth)
{
    float rel = abs(tap_depth - center_depth) / max(max(tap_depth, center_depth), 1.0);
    return exp(-rel * 64.0);
}

vec3 gatherScatter(vec2 uv, float center_depth)
{
    // Source texel containing this display pixel, and the 4x4 window
    // floor-1 .. floor+2 around it. Works for both half-res and full-res
    // sources; at full res the window is the pixel's own 4x4 Bayer block
    // neighbourhood.
    vec2 src = uv / emissiveRectDelta - 0.5;
    vec2 base = floor(src);
    vec2 f = src - base;
    vec2 min_uv = emissiveRectDelta * 0.5;
    vec2 max_uv = vec2(1.0) - min_uv;
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 2; ++y)
    {
        // Tent in y: distance from the sub-texel position, width 2 texels.
        float wy = max(0.0, 2.0 - abs(float(y) - f.y));
        for (int x = -1; x <= 2; ++x)
        {
            float wx = max(0.0, 2.0 - abs(float(x) - f.x));
            vec2 tap_uv = clamp((base + vec2(float(x), float(y)) + 0.5) * emissiveRectDelta,
                                min_uv, max_uv);
            float w = wx * wy * depthWeight(linearViewDepth(tap_uv), center_depth);
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

`main()` for mode 0 and mode 1 then becomes:
`float d = linearViewDepth(vary_fragcoord); vec3 s = gatherScatter(vary_fragcoord, d);
frag_color = vec4(s, compositeTransmittance(d));`. Modes 2/3/10/11 keep the
plain `texture()` sample. Delete `depthSimilarity`, `normalSimilarity`,
`depthAwareScatterBlur`, the `depthAwareUpsample`, `scatterBlurStrength`
and `scatterBlurRadius` uniforms, and the `NORMAL_MAP` bind in
`draw_composite`. The tent weights already give bilinear reconstruction at
Normal, so no separate upsample step is needed.

Cost note: 32 fetches/pixel at 8.4M pixels is ~0.4-0.6 ms in High. If that
shows up in timing, make it separable: a horizontal 4-tap pass into a
second source-res target, vertical 4-tap in the composite (8+8 fetches).
Start with the single 16-tap gather; it is simpler.

Debug: keep modes 2/3 unfiltered; apply the gather in mode 1.

## 4. Directional Raymarch Speed-Ups (in priority order)

### 4.1 One shadow fetch per step (largest win)

`asVolumetricPCFShadow()` in `asVolumetricShadowUtil.glsl`: replace the
five `texture()` calls and the `stc.x = floor(...)` snap with a single
`texture(shadow_map, stc.xyz)`. The cascades are bound `TFO_BILINEAR` with
compare mode, so one fetch is already a hardware 2x2 PCF. The snap line
exists upstream only to disguise the 5-tap pattern; without it, bilinear
compare gives a smoother result. Keep `stc.xyz /= stc.w` (harmless) and the
`shadow_bias * 2.0` term.

If 1 tap looks noisier than acceptable at cascade 0 near contact shadows,
try 2 taps offset by (+0.5,+0.5)/(-0.5,-0.5) texels alternating per pixel
parity. Do not go back to 5.

Expected: directional -60..-75% (High 9.7 -> ~3 ms).

### 4.2 Single-cascade selection (no cross-fade)

In `asVolumetricDirectionalShadow()`, replace the four overlapping
`if` blocks with one selection: cascade 3 if `z < -shadow_clip.z`, else 2 if
`z < -shadow_clip.y`, else 1 if `z < -shadow_clip.x`, else 0. Keep the
far-cascade fade term for cascade 3 and the `<= -shadow_clip.w -> 1.0`
early return. Weight normalisation disappears.

Rationale: cross-fade prevents seams on surfaces; the volumetric integral
plus interleaved filter already blends across depth, so a hard switch
inside fog is invisible in practice. Risk: a faint band at a split where
shadow resolution changes. Guard the change with `#define AS_VOL_SINGLE_CASCADE 1`
so it can be A/B'd without touching the surface sampler.

Expected: -20..-30% additional in the near/mid range where overlaps sit.

Reference implementation for 4.1 + 4.2 together (whole body of
`asVolumetricShadowUtil.glsl` after the uniform declarations; keep the
existing `#if defined(SUN_SHADOW)` guard and uniforms):

```glsl
#define AS_VOL_SINGLE_CASCADE 1

float asVolumetricShadowFetch(sampler2DShadow shadow_map, vec4 stc)
{
    stc.xyz /= stc.w;               // orthographic: w == 1, kept for safety
    stc.z += shadow_bias * 2.0;
    return texture(shadow_map, stc.xyz); // hardware bilinear 2x2 compare
}

float asVolumetricDirectionalShadow(vec3 sample_pos, vec2 pos_screen)
{
    vec4 spos = vec4(sample_pos, 1.0);
    if (spos.z <= -shadow_clip.w)
    {
        return 1.0;
    }
#if AS_VOL_SINGLE_CASCADE
    float shadow;
    if (spos.z < -shadow_clip.z)
    {
        shadow = asVolumetricShadowFetch(shadowMap3, shadow_matrix[3] * spos);
        // Same far fade as upstream so visibility reaches 1 at shadow_clip.w.
        shadow += max((spos.z + shadow_clip.z) /
                      (shadow_clip.z - shadow_clip.w) * 2.0 - 1.0, 0.0);
        return clamp(shadow, 0.0, 1.0);
    }
    if (spos.z < -shadow_clip.y)
    {
        return asVolumetricShadowFetch(shadowMap2, shadow_matrix[2] * spos);
    }
    if (spos.z < -shadow_clip.x)
    {
        return asVolumetricShadowFetch(shadowMap1, shadow_matrix[1] * spos);
    }
    return asVolumetricShadowFetch(shadowMap0, shadow_matrix[0] * spos);
#else
    // Previous cross-faded four-cascade body, with asVolumetricPCFShadow
    // replaced by asVolumetricShadowFetch (still 4.1).
#endif
}
```

Note the split thresholds: cascade i covers `z > -shadow_clip[i]`
(view-space z is negative forward). The upstream cross-fade used
0.75/1.25 multiples; the hard split sits at the nominal plane, i.e. the
middle of the old blend band.

### 4.3 Fewer steps per pixel with interleaved sampling (section 3 required)

With Bayer 4x4 in place, set `getSampleCount()` to 8 Normal / 12 High
(High effective 192 samples per 4x4 block, Normal 128). Keep
`RenderVolumetricLightingSampleCountOverride` (4..32) for tuning. Keep the
`min_steps = 4` near-geometry floor. Check open-sky shafts and roofline
shafts at 8, 12, 16 in High; pick the lowest count with no visible
banding in motion.

Expected: High -60..-75% of what remains after 4.1 (~3 -> ~1 ms).

### 4.4 Shadow-space recurrence (optional, after 4.1-4.3 are measured)

Per pixel and per cascade, compute once
`s0 = shadow_matrix[c] * vec4(sample_pos0, 1)` and
`ds = shadow_matrix[c] * vec4(sample_step, 0)` (orthographic, so `w == 1`
and no divide), then advance `s += ds` per step and switch `c` when
`sample_pos.z` crosses a split (recompute `s` at the switch only). Saves a
mat4 multiply + divide per step. Worth ~5-10% once the shader is no longer
purely fetch-bound. Skip if 4.1-4.3 already meet the target.

### 4.5 What NOT to do

- No more scalar arithmetic micro-rewrites (already measured as no gain).
- No checkerboard/half-step skipping without temporal reconstruction.
- No reduction of atlas slices or atlas refresh (0.28 ms; visible when
  stale).
- Do not touch upstream `shadowUtil.glsl`.

## 4b. Memory (VRAM / RAM)

Current footprint at 3440x1440 (from `allocateResources()` / `renderPass()`):

| Resource | Format | High | Normal |
|---|---|---:|---:|
| `sVolumetricTarget` | RGBA16F, no depth | 39.6 MB | 9.9 MB |
| `sTransparencyAtlas` (half res, 4x4 tiles) | RGBA16F | 9.9 MB | 9.9 MB |
| `sAtlasIntegralTex[2]` | R16F x2 | 5.0 MB | 5.0 MB |
| Blue-noise texture | RGBA8 1024x1024, no mips | 4.2 MB VRAM + ~4 MB RAM decoded + 4.2 MB x2 on disk | same |
| Total VRAM | | ~59 MB | ~29 MB |

Reductions, in order of value:

1. Delete the blue-noise path (section 3): -4.2 MB VRAM, -4 MB RAM,
   -8.4 MB install size (two identical PNGs). Lossless once Bayer is in.
2. `sVolumetricTarget` to `GL_R11F_G11F_B10F`: -19.8 MB High, -5 MB
   Normal, plus the bandwidth win already noted in 2.6. Quality trade is
   the dim-moonlight banding test in section 1b.
3. `sAtlasIntegralTex`: keep two (ping-pong is required), but they can be
   `GL_R16F` at the atlas's own tile resolution only if the shader's
   `prev_uv` lookup is adapted; not worth the complexity. Leave.
4. `sTransparencyAtlas` must keep alpha (transmittance), so it stays
   RGBA16F. Do not change.
5. Release `sVolumetricTarget`, the atlas, and the integral textures when
   `RenderVolumetricLighting` is turned off at runtime: today they persist
   until the next resolution change. Add a release in `renderPass()`'s
   `!isEnabled()` early-out when the targets are allocated, and
   re-allocate lazily (the existing size check in `renderPass()` already
   handles re-allocation of `sVolumetricTarget`; add the same for the
   atlas). Saves the full 29-59 MB for users who disable the feature.
   Lossless.
6. Temporary CPU vectors in `renderLocalLights()` are tiny; nothing to do.

RAM otherwise: no persistent CPU-side buffers exist beyond the fetched
noise image and settings. Nothing further to reduce.

## 5. Local Lights (unchanged priority: low, off by default)

Measured 0.07-0.26 ms when active. If ever needed: draw one screen-space
bounding quad per light instead of a full-screen loop, and reduce
`LOCAL_STEPS` to 4. Not part of this pass.

## 6. Temporal Accumulation (deferred; do not implement now)

Would allow 2-4 steps per pixel per frame. Requirements absent from the
codebase: previous-frame view-projection matrix per eye, a persistent
history target pair at source resolution, reprojection with depth-based
disocclusion rejection, invalidation on camera cut / sun-moon switch /
shadow-map refresh, and clamping against ghosting behind avatars and
foliage. Expected cost: several days of iteration; expected visible risk:
smearing under motion. Revisit only if sections 3-4 fall short of the
user's FPS goal. Moment shadow maps (previous doc) remain a separate
research phase.

## 6b. Reference Shaders Reviewed (repo root)

Reviewed after the plan was drafted; nothing here changes the plan, three
items confirm it, one adds an optional local-light idea.

| File | What it does | Relevance |
|---|---|---|
| `VolumetricLight.shader` (Skalsky, Unity) | Directional pass: ONE hardware shadow fetch per step (`UNITY_SAMPLE_SHADOW`), cascade chosen by split spheres with NO cross-fade, per-pixel offset from a 4x4 or 8x8 dither texture (`DITHER_4_4`), phase applied once outside the loop, extinction accumulated inline. | Directly confirms 4.1, 4.2 and section 3. This is a shipped, widely used implementation using exactly the combination proposed. Its `_MieG` precompute (1-g², 1+g², 2g, 1/4π) is a trivial optional ALU save in `phaseHG`; not worth a separate step. |
| `.MSMBeyondHardShadowsCode/ParticipatingMedia.fx` `ComputeSingleScatteringRayMarchingDirectional` | Transforms ray start/end into shadow projection space once, advances `SamplePosition += SampleOffset`, one unfiltered fetch per step, exact segment weights via `Weight *= WeightFactor`. | Confirms 4.4 (shadow-space recurrence) and the already-adopted exact Beer-Lambert segment weights. Its 128 samples and single map are not comparable to our cascades; do not copy sample counts. The moment-shadow-map parts remain the separate research phase of the previous doc. |
| `vol.light2.shader` (Hillaire, Frostbite) | `Sint = (S - S*exp(-σe*dd))/σe` improved integration; volumetric shadow by marching toward the light. | The integration is already implemented in `asVolumetricLightF.glsl`. Marching toward the light is for media self-shadowing without shadow maps; not applicable. Nothing to take. |
| `bluenoisefog.shader` (Demofox) | Compares fixed / white / blue / IGN offsets, with frame animation `fract(bn + frame*0.618)`. Running mean `mix(acc, vis, 1/(i+1))`. | Same conclusion as the previous doc: animated noise requires temporal accumulation, which we lack. Static blue noise is shown against a large-kernel eye average, not a 3x3 filter; supports section 3's reasoning. Nothing to take. |
| `vol.light.shader` (Aoki, analytic volumetric lighting) | Closed-form line integral of an inverse-square point light along a ray: `L = γI·J·(atan(J(dt+x0)) - atan(J·x0))`, `J = 1/√(y0²)` after rotating the ray into a light-aligned frame. Zero samples. | **Optional idea for `asVolumetricLocalLightF.glsl`.** Our local lights are unshadowed, so an analytic integral is exact. It requires changing the falloff from the current artistic `(1-d/r)^p` to inverse-square with a smooth radius window; that changes the look of local fog and is therefore a trade, not lossless. Local lights are off by default and cost 0.07-0.26 ms, so this is Phase D material only if the user wants local lights cheaper. Do not do it in this pass. |

Summary: no reference contradicts the plan. Skalsky's shader is the
closest production analogue and uses 1-tap + hard cascade + 4x4 dither
together, which is the strongest evidence that 4.1, 4.2 and section 3 are
visually acceptable in combination.

## 7. Implementation Order and Validation

Work in phases. Each phase ends with one build by the user. Do not start
a later phase before the earlier one is `bokt` (built and runtime-tested).
Within a phase, do the steps in the listed order; later steps assume
earlier ones are in place.

**Phase A - diagnosis (1 build, tiny diff)**

1. 2.1 blue-noise binding fix only. Nothing else. User builds, tests at
   `BlueNoiseStrength = 1`, debug mode 2, Normal quality. Ghost must be
   gone. If it is not, stop and report; the rest of the plan still
   applies but the diagnosis note in section 2.1 must be corrected.

**Phase B - shader core (1 build)**

2. 4.1: single fetch in `asVolumetricShadowUtil.glsl`, keep the four
   cross-faded cascades for now (so 4.1 is measured alone).
3. 4.2: add the `AS_VOL_SINGLE_CASCADE` path, define set to 1.
4. Section 3, shader side: Bayer jitter in `asVolumetricLightF.glsl`;
   delete the blue-noise fetch and both blue-noise uniforms; Bayer jitter
   in `asVolumetricAtlasF.glsl`.
5. 2.4: delete the ten dead uniforms from both shaders (do this after
   step 4 so you are not editing the same declarations twice).
6. Section 3, composite side + 2.5: replace the composite's `main()`
   filter path with the gather; add `zNear/zFar` uniforms; delete the
   normal guide and blur uniforms.
7. C++ side of steps 4-6 in `asvolumetriclighting.cpp`: remove blue-noise
   load/bind/uniform, remove `applyMoonAppearance()` and its calls, remove
   blur uniform uploads and the `NORMAL_MAP` bind in `draw_composite`,
   upload `zNear/zFar`, set `getSampleCount()` to 8/12 (4.3).
8. Settings/XUI/texture cleanup: remove `BlueNoiseStrength`,
   `BlurStrength`, `BlurRadius` from `settings.xml` and any XUI; remove the
   `ASBlueNoise` entry from `textures.xml`; list both PNGs for the user to
   delete. Grep `indra/newview` for every removed name before finishing.
9. Set `AS_VOLUMETRIC_PERFORMANCE_LOGGING 1`. User builds and captures.

**Phase C - CPU and memory (1 build, no visual change)**

10. 2.3 per-frame cache for `bindTransparencyAtlas()`; `LLCachedControl`
    in `isEnabled()`.
11. 4b item 5: release targets when the feature is disabled, lazy
    re-allocate.
12. 2.6 / 4b item 2: `R11F_G11F_B10F` for `sVolumetricTarget` (optional;
    include only if the user accepted the moonlight banding test).
13. Set `AS_VOLUMETRIC_PERFORMANCE_LOGGING 0`. User builds.

**Phase D - optional, only if Phase B misses the target**

14. 4.4 shadow-space recurrence.
15. Revisit step counts (4.3) with the override setting.

Measurement after Phase B (step 9), before Phase C:

- Measure at the controlled camera (3440x1440): Disabled / Normal / High,
  atlas active and inactive. Record the four stage averages. Compare
  against 55 / 49 / 34 FPS and the 1.5 / 9.7 ms directional baselines.
- Visual checks: cascade splits (4.2), contact shadows and foliage (4.1),
  open sky and roof shafts at 8/12/16 steps (4.3), camera rotation and
  avatar motion (section 3 filter), transparent foliage/water vs opaque
  match (atlas jitter change), debug modes 1/2/3/10/11.
- If a cascade seam is visible, set `AS_VOL_SINGLE_CASCADE 0` and note it
  in the report. If 12 steps band in High, use 16.
- Only then proceed to Phase C, and decide Phase D.

Acceptance: High directional <= 1.5 ms at 3440x1440, no lattice bands, no
silhouette ghosts, no cascade seams visible in motion, Normal not slower
than today.

## 8. File-Level Checklist for the Implementing Agent

- `asvolumetriclighting.cpp`: 2.1 binding; 2.3 per-frame cache; 2.4 remove
  `applyMoonAppearance`; `getSampleCount()` defaults 8/12; remove blur and
  blue-noise settings plumbing once section 3 is in; optional target format.
- `asvolumetriclighting.h`: new private statics for 2.3; remove
  `sBlueNoiseImage` usage.
- `asVolumetricShadowUtil.glsl`: 4.1 single fetch; 4.2 single cascade.
- `asVolumetricLightF.glsl`: section 3 Bayer jitter; delete unused uniforms
  and the blue-noise uniforms.
- `asVolumetricAtlasF.glsl`: delete unused uniforms; Bayer jitter on
  `gl_FragCoord.xy + slice_index` offset.
- `asVolumetricCompositeF.glsl`: 2.5 cheap depth; section 3 single 4x4
  depth-aware gather; remove normal guide, blur uniforms.
- `settings.xml`, `panel_as_volumetric_lighting.xml`,
  `floater_as_volumetric_lighting.xml`: remove blur/blue-noise controls if
  exposed; keep `SampleCountOverride` (non-persistent).
- `textures.xml` + both `as_blue_noise.png` files: remove (user deletes
  files with git).
- No `ll*`/`fs*` edits are needed for this pass. If any become necessary,
  wrap them in `<AS:Chanayane>` tags per `AGENTS.md`.
