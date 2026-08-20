# AyaneStorm Volumetric Lighting (God Rays) — Implementation Plan

## Fresh audit correction (2026-08-20)

The complete feature diff for commit
`0e6eb17bf728264ad2b0eddeec39e290f18938ce` was reviewed against its parent,
not merely its file statistics. This confirmed that the late
`renderFinalize()` integration and inverted minimum-visibility calculation
were present in the committed implementation itself. The resource lifecycle,
shader-manager hooks, settings, feature-table entries, and preferences wiring
did not explain the observed all-far depth result.

The earlier handoff treated a valid `depthMap` sampler returning `1.0` as a
shader reconstruction problem. That conclusion did not follow from the
evidence: a valid texture binding says nothing about whether the shared depth
attachment still contains the scene depth at that late point in the frame.

The pass has therefore been moved from `LLPipeline::renderFinalize()` to
immediately after `renderDeferredLighting()`, while the deferred G-buffer is
still the active, authoritative source for the frame. The raymarch shader also
binds `mRT->deferredScreen`'s depth attachment explicitly, matching the working
deferred post-process binding pattern.

A separate lighting-equation error was corrected. The old shader accumulated
the darkest shadow sample and inverted it, causing shadowed air/occluders to
produce light while open illuminated air produced none. The pass now integrates
mean directional-light visibility along the view ray, and uses the moon color
when the moon is the active directional light. Shader cache revision `v2`
invalidates binaries compiled from the previous GLSL.

This source audit was intentionally not followed by a build: repository policy
requires the user to perform builds. Runtime verification should begin with
debug mode 6; nearby geometry must no longer read uniformly white. Then verify
mode 3 shows varying mean visibility before testing normal mode 0.

### Build verification log

- **2026-08-20, first build after the fresh audit: failed.** MSVC reported
  C2100 at `llviewerdisplay.cpp:1181` because the new call dereferenced
  `gPipeline.mRT->screen`, which is already an `LLRenderTarget` object rather
  than a pointer. The call now passes `gPipeline.mRT->screen` directly.
- Rebuild and runtime verification remain pending.

### Runtime verification round after depth-lifetime fix

The next build succeeded and the supplied screenshots materially changed the
diagnosis:

- **Mode 7:** uniform deep blue. This is a valid result for one fixed NDC input
  across every fragment and demonstrates that `inv_proj` is populated and
  non-identity.
- **Mode 4:** contains a recognizable avatar silhouette plus perspective-shaped
  X/Y color bands. Scene geometry is therefore present in reconstructed
  positions; the old conclusion that `getPosition()` merely passed through NDC
  was incorrect.
- **Mode 1:** mostly dark with localized bright signal around the windows. The
  raw scatter has spatial structure consistent with directional visibility, so
  depth and shadow sampling are now operating end-to-end.
- **Mode 0:** severe nearly full-frame whiteout. Since mode 1 is structured, the
  remaining failure is signal magnitude in the HDR additive composite rather
  than missing depth.
- **Mode 2:** reported all white.
- **Mode 3:** screenshot shows a large white region and dark foreground with a
  clear geometry boundary. The camera was inside a room, with a window behind
  the avatar and a door to camera-right; the white region must therefore not be
  assumed to be sky. Mean shadow visibility is spatially non-uniform, but the
  large fully-visible interior region may still indicate incorrect shadow
  classification or sampling outside a valid cascade footprint.
- **Mode 5:** all white because the tested view rays reach the shader's 128 m
  march-distance cap; this does not diagnose a depth failure.
- **Mode 6:** all white. Raw device depth is nonlinear and naturally packed
  very close to 1.0 for most view-space distances under a perspective
  projection. Treating this as proof of a far-plane sample was an invalid
  diagnostic.

Shader revision `v3` applied `SCATTER_RADIANCE_SCALE = 0.02` before multiplying the
normalized visibility integral into the HDR directional-light color. This
was a diagnostic magnitude reduction; the subsequent controlled test and its
reversion are recorded below.

A matching baseline screenshot with volumetric lighting disabled was supplied
after the initial interpretation. It shows that mode 3's major boundary follows
the room floor versus the walls/ceiling rather than a horizon. Mode 1's localized
bright regions also line up with the rear window and the door/opening at the
right. This scene-grounded comparison makes invalid cascade coverage less likely
and supports the raw visibility signal being coherent. The immediate remaining
test is whether shader revision `v3`'s radiance scale fixes mode 0 without erasing
the opening-aligned structure visible in mode 1.

### Runtime verification of shader revision v3

Controlled indoor and outdoor triplets (disabled, mode 0, mode 1) showed that
the `0.02` RGB scale made mode 1 approximately 50 times darker, but mode 0 still
produced the same full-screen whiteout. Scatter RGB magnitude was therefore not
the cause, and the v3 scale has been reverted.

The composite used `LLRender::BT_ADD`, whose blend factors are `ONE, ONE` for
both RGB and alpha, while `asVolumetricCompositeF.glsl` output alpha `1.0`.
This added one into the HDR screen target's alpha channel before later
screen-space-reflection and finalization passes consumed that target. The
composite now outputs alpha zero and masks alpha writes around the draw,
preserving the existing screen alpha while additively accumulating RGB only.
Shader cache revision is now `v4`; indoor/outdoor mode 0 and mode 1 verification
is pending.

### Runtime verification of shader revision v4

Controlled indoor and outdoor triplets confirm the alpha-preservation fix:

- Mode 0 no longer whiteouts and remains visually close to the disabled
  baseline.
- Outdoor mode 1 contains coherent visibility shafts shaped by the house and
  tree occluders.
- Indoor mode 1 contains faint but recognizable rays aligned with the rear
  window/opening.
- Normal mode 0 is currently very subtle at the persisted default
  `RenderVolumetricLightingIntensity = 0.5`, particularly indoors. This is now
  an artistic/default-strength tuning issue rather than a broken rendering
  pipeline.

Before changing the shipped default or adding an intensity control to the
preferences panel, test `RenderVolumetricLightingIntensity = 2.0` live through
Debug Settings in mode 0. No rebuild is required for this cached setting.

Per user decision, intensity is now exposed as a live Preferences slider under
AyaneStorm > Rendering. It controls the existing persisted
`RenderVolumetricLightingIntensity` setting over `0.0` to `5.0` in `0.1` steps
and is disabled when volumetric lighting is off. The shipped default remains
`0.5`; users can choose a stronger look without Debug Settings or a rebuild.

The volumetric debug spinner is intentionally retained. The same Rendering
panel now also exposes the existing Exact OIT diagnostic modes `0..9` and
AVBOIT diagnostic modes `0..15`. Each spinner is enabled only while its
corresponding transparency mode is active and includes an exhaustive mode list
in its tooltip. These controls bind directly to `RenderExactOITDebugMode` and
`RenderAVBOITDebugMode`; no rendering implementation was changed.

On macOS, the transparency selector, both OIT diagnostic labels, and both OIT
diagnostic spinners are hidden together. The Rendering tab itself remains
visible so its GL 4.1-compatible volumetric lighting controls remain available;
macOS users see no Exact OIT or AVBOIT UI.

Runtime inspection of the Rendering preferences panel found that the two OIT
spinners overlapped vertically because their relative `top_delta` positioning
chained through adjacent controls, while the volumetric debug spinner was
pushed farther right by a wider label. The diagnostic rows now use explicit
vertical positions and a common spinner column at x=250; all three labels use
the same width and alignment.

## Optional local-light volumetrics — feasibility note

### First experimental implementation (2026-08-20)

The opt-in first version is now implemented for runtime evaluation:

- `RenderVolumetricLocalLights` is disabled by default and requires the main
  volumetric-lighting toggle.
- A separate half-resolution local-light shader evaluates up to eight selected
  nearby lights in one draw. It intersects the reconstructed camera ray with
  each light sphere and integrates eight samples only across the chord in front
  of visible scene depth, rather than illuminating the entire view ray.
- Candidates reuse `LLPipeline::mNearbyLights` filtering/fading and are scored
  by linear brightness, radius, and camera distance. The configurable count is
  a hard safety ceiling, not a claim that four or eight is a renderer limit.
- Point lights and spotlights currently share the same spherical, unshadowed
  approximation. This should make street lamps visibly illuminate fog, but it
  can leak through walls and does not yet reproduce a projector cone.
- Preferences expose the local enable toggle, independent intensity
  (`0..2`, default `0.35`), and maximum evaluated lights (`0..8`, default `8`).
- Existing directional debug modes remain isolated: local-light accumulation
  is skipped whenever `RenderVolumetricLightingDebug` is nonzero.
- The initial experimental shader cache revision was
  `as-volumetric-lighting-v5`.

Build and runtime validation are pending. First test with the same street-lamp
scene, debug mode 0, main volumetrics enabled, then toggle **Include local
lights (unshadowed)** without moving the camera. If the effect is absent, check
`AyaneStorm.log` for `AS Volumetric Local Light Shader`. If it is excessive,
lower **Local intensity** before changing the shader equation. Also test from
inside a closed room to quantify the expected unshadowed leakage.

### First runtime result and intensity correction

The pass builds and visibly responds to nearby lights, confirming that the
light snapshot, selection, coordinate transform, depth limit, and additive
composite are operating. The first comparison was much too strong even at UI
intensity `0.05`. Local-light colors are direct-light scene radiance, while the
fog approximation integrates them across a light-volume chord; sending that
integral unnormalized into the HDR target was therefore badly scaled.

Shader revision `v6` applies a `0.02` local-scatter radiance normalization
(50x reduction) after integration. This is deliberately local to the new pass
and does not alter the already validated sun/moon contribution. The local
intensity slider increment is now `0.01` for finer live tuning. Rebuild and
repeat the fixed-camera off/on comparison with the saved intensity unchanged;
then try `0.25`, `0.5`, and `1.0` if necessary.

Both the directional and local intensity sliders now allow direct numeric text
entry (`can_edit_text=true`) in addition to mouse dragging.

After the `v6` normalization test, local intensity `2.0` was judged a useful
value that should sit around one third of the adjustment range. The local-only
slider maximum is therefore `6.0` (still with `0.01` steps and direct entry);
the directional sun/moon intensity range remains unchanged at `0..5`.

Runtime tuning selected shipped defaults of `0.8` for directional sun/moon
volumetrics and `0.35` for local-light volumetrics. Existing persisted user
values are intentionally preserved; these values apply to fresh or reset
settings.

The Rendering preferences layout was subsequently reorganized into distinct
**Transparency / OIT** and **Volumetric lighting** sections separated by a
horizontal divider. Within the volumetric section, the diagnostic mode now
sits immediately after the sun/moon enable and intensity controls; optional
local-light controls follow it as one contiguous subgroup. The main labels now
say “Enable sun/moon god rays” and “Sun/moon intensity” to distinguish them
from local-light settings.

This revision was checked against the supplied 494x455 preferences screenshot,
not inferred solely from XML. The screenshot specifically showed the OIT and
volumetric rows reading as one uninterrupted list and the volumetric debug row
stranded near the panel bottom after a large blank gap. The divider is sized to
the visible panel width, and the reordered debug row removes that gap.

The same screenshot also showed that the maximum-local-lights selector was not
visible below Local intensity. The runtime log contained no control-binding or
XUI creation error. Its embedded spinner label was replaced with an explicit
text label plus a 60-pixel spinner in the common x=250 control column, matching
the proven OIT and volumetric debug-row structure.

Initial runtime testing showed no observable performance impact at the original
eight-light ceiling on the test GPU. Revision `v7` therefore raises the
configurable hard ceiling and shader arrays from 8 to 32 lights while retaining
8 as the conservative shipped default. This permits hardware-dependent scaling
without silently quadrupling the default per-pixel worst-case workload.

Local-light support is feasible but is not a trivial extension of the current
directional pass. `LLPipeline::renderDeferredLighting()` already gathers,
distance-sorts, fades, and caps local lights through `mNearbyLights`, with point
light position/radius/color/falloff and spotlight projector data available.
However, the renderer only maintains two projected spotlight shadow maps
(`mSpotShadow[0..1]`). Ordinary point lights have no omnidirectional shadow map,
and most spotlights therefore also lack shadow data in a given frame.

Recommended scope for an optional first implementation:

- Add a separately controlled local-volumetric pass, disabled by default.
- Reuse the existing nearby-light ordering and expose a configurable safety
  ceiling (for example `0..16`, with a conservative default around 4 or 8).
  Four is not a technical limit and should not be treated as the primary work
  budget.
- Select lights primarily by estimated GPU work: projected screen coverage,
  brightness, distance, overlap, shadow availability, and sample count. This
  permits many small street lamps while rejecting a few nearby lights whose
  volumes would cover most of the screen.
- Restrict each draw/raymarch to the light's screen-space bounding volume.
- Support point lights and unselected spotlights as explicitly **unshadowed**
  fog glow, with depth-limited distance attenuation. Such light cannot produce
  physically occluded shafts and may leak through walls.
- Use real occlusion only for the at-most-two spotlights selected into the
  existing projected shadow maps.
- Reduce samples for small or distant light volumes and enforce a total
  per-frame workload budget in addition to the configurable count ceiling.
- Expose separate enable, intensity, maximum-light-count, and workload-budget
  settings so this cost and the unshadowed approximation are opt-in.

This is medium-to-high complexity: it needs a new light-data handoff from
`LLPipeline` into the AS-owned module, new point/spot raymarch shader variants,
bounded-volume rendering, projected-shadow integration, performance controls,
and indoor leakage testing. It should remain a separate follow-up feature rather
than being folded casually into the now-working sun/moon pass.

## Handoff note (read this FIRST if picking this up cold)

The feature is code-complete and builds, but is **not visually working**: the raymarch's reconstructed view-space depth reads as pegged-at-far-plane almost everywhere, even pointed at geometry a few meters from the camera (confirmed via debug mode 6, see Round 10/11 below). As of Round 11, the bug has been narrowed to: **`depthMap` IS bound to a real, valid texture channel (channel 0, confirmed via a runtime log), yet `getDepth(pos_screen)` — a direct, unconditional call with no math in between — still reads back `1.0`/far-plane everywhere in that same test.** Every other piece of the chain (shader attachment order, `inv_proj` non-degenerate per debug mode 7, vertex shader UV convention byte-identical to working shaders like `softenLightV.glsl`, texture-unit binding call correctness) has been individually verified correct through static reading. This combination — valid channel, wrong sampled content — could not be resolved further through source reading alone and needs one of:
- A RenderDoc/NSight capture of the actual draw call to see what texture is really bound to unit 0 and what it contains at that point in the frame (the fastest real next step — this investigation has exhausted what static code reading can determine).
- Or: try binding `depthMap` manually in `ASVolumetricLighting::renderPass()` (bypass `bindDeferredShader`'s automatic binding, `gGL.getTexUnit(N)->bind(&mRT->deferredScreen, true)` directly, matching e.g. `pipeline.cpp:8453`'s `gDeferredPostNoDoFProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &mRT->deferredScreen, true)` pattern) to see if that changes anything — would help distinguish "the depth texture object itself has the wrong content at this point in the frame" from "something specific to going through `bindDeferredShader`'s generic path is wrong."
- A secondary, confirmed-real, separate bug worth fixing regardless: `sun_dir`/`moon_dir`/`sunlight_color` are never populated for these shaders because they're not registered in `LLViewerShaderMgr::mShaderList` (see Round 10 for the full mechanism) — fix by registering both `gASVolumetricLightProgram`/`gASVolumetricCompositeProgram` there, or by manually pushing those three uniforms in `renderPass()` every frame.

Debug infrastructure already in place to help: `RenderVolumetricLightingDebug` (0-7) in AyaneStorm prefs → Rendering tab. Modes 4-7 were added specifically for this investigation (see Round 10) and isolate position reconstruction from shadow sampling entirely — reuse them rather than re-deriving from scratch. All debug code is TEMPORARY and tagged as such in both the `.glsl` and `.xml`; strip it once the real bug is fixed and confirmed working.

## Status (update as work proceeds — read this first if resuming)

Legend: `[ ]` todo · `[~]` in progress · `[x]` done

- [x] **Step 1** — `ASVolumetricLighting` module (`indra/newview/asvolumetriclighting.h/.cpp`)
- [x] **Step 2** — `RenderVolumetricLighting`/`...Intensity`/`...Asymmetry` settings in `app_settings/settings.xml`
- [x] **Step 3** — Render target alloc/release call-outs in `pipeline.cpp` (`:1022-1024`, `:1434-1436`)
- [x] **Step 4** — Shaders: `asVolumetricLightV/F.glsl`, `asVolumetricCompositeF.glsl`
- [x] **Step 5** — Shader registration in `llviewershadermgr.cpp` (load/unload/cache-revision hash, `#include`)
- [x] **Step 6** — Hooked `ASVolumetricLighting::renderPass()` into `LLPipeline::renderFinalize()` (`pipeline.cpp:8963-8966`)
- [x] **Step 7** — `RenderVolumetricLighting` entries in `featuretable.txt`, `featuretable_mac.txt`, `featuretable_linux.txt` (`all` + `safe` lists)
- [x] **Step 8** — Preferences UI checkbox in `ASPanelPrefsAyaneStorm` (see "Preferences tab decision" below — the Mac tab-hiding logic changed)
- [x] **Step 9** — Registered `asvolumetriclighting.h/.cpp` in `indra/newview/CMakeLists.txt`
- [ ] **Not started** — Build verification (user builds; the agent that did this work cannot build). See Verification section below for the test plan once a build is available.

**All implementation steps are code-complete. The build succeeds.** Testing history:
- Round 1 (average-visibility scatter, raw-meters distance bug): near-total whiteout.
- Round 2 (occlusion-based scatter): still overexposed, plus new directional streaking near depth discontinuities. Root cause suspected (and fixed, unconfirmed): `renderPass()` never explicitly set GL depth-test/blend state before its draws, inheriting whatever state `renderFinalize()`'s caller happened to leave active. Now explicitly forced via `LLGLDepthTest depth(GL_FALSE, GL_FALSE)` + per-draw `LLGLDisable`/`LLGLEnable blend`.
- Round 3 (debug modes added): debug mode **1** (scatter with color/intensity, screen replaced) showed a *plausible-looking but likely inverted* image — window openings (unoccluded paths to sky) were the brightest spots, when they should be near-zero occlusion/dark. Debug mode **2** (raw occlusion) came back "all white" — traced to a real bug in the debug plumbing: the composite pass only special-cased `debug_mode == 1` for `BT_REPLACE`; mode 2 fell through to `BT_ADD` and was blending onto the already-bright scene. Fixed to `debug_mode != 0`.
- Round 4 (after the mode-2 compositing fix, before the cascade-mapping fix below): mode 2 still 100% white, mode 1 unchanged; mode 3 (raw `min_visibility`, pre-inversion) came back **solid black**, confirming `sampleDirectionalShadow()` really was returning ~0 (fully shadowed) for essentially every raymarch sample everywhere, not a compositing artifact.
- Round 5 (root cause found by reading `sampleDirectionalShadow()`'s own branch structure line-by-line rather than guessing further): **the cascade-index-to-`shadow_clip`-component mapping was backwards.** `sampleDirectionalShadow`'s first-checked branch (`spos.z < near_split.z`, the *nearest-to-camera* condition among its four branches, since `spos.z` is negative view-space depth and smaller `|spos.z|` = closer) uses `shadow_matrix[3]`/`shadowMap3`. Its last/catch-all branch (`spos.z > far_split.x`, the *nearest* condition, closest to 0) uses `shadow_matrix[0]`/`shadowMap0`. So **index 0 is the NEAREST cascade** (bounded by `shadow_clip.x`) **and index 3 is the FARTHEST** (bounded by `shadow_clip.w`) - confirmed by direct symbolic substitution with representative increasing `shadow_clip` values, not assumed. An earlier version of `asvolumetriclighting`'s replacement single-cascade sampler (see below) had this exact mapping backwards, meaning near-camera raymarch samples (the vast majority of them, since `MAX_MARCH_DISTANCE` is only 128m and most samples are much closer) were being sent to `shadowMap3` - the shadow map rendered for the *farthest* cascade, whose footprint does not cover near-camera geometry at all. Reading a `sampler2DShadow` outside its rendered/valid region very plausibly reads as "in shadow" (0.0) depending on the depth-compare/clamp behavior at the map edges, which would explain occlusion reading as ~1.0 (i.e. min_visibility ~0.0) almost everywhere, including toward open windows.

- Round 6 (retested round 5's fix - **and it was never actually running**): a `diff` between the source tree and the build's staged `app_settings` directory showed the build output was stale (missing the round-5 rewrite entirely), so round 5 was never really tested. After a real rebuild + shader cache wipe, the corrected shader **failed to compile**: `AyaneStorm.log` showed `error C1503: undefined variable "shadowMap0"` (and `1`/`2`/`3`) at link time, and `LLGLSLShader::createShader : Failed to link shader: AS Volumetric Light Shader`. Because `loadShaders()` treats a compile failure as "feature unavailable" (`sShadersLoaded = false`) rather than crashing, this manifested as "volumetric lighting has no visible effect at all, including all debug modes" - not a rendering bug, a link failure being handled gracefully.
- Round 7 (root cause of the link failure, found by tracing `LLShaderMgr::attachShaderFeatures`/`LLGLSLShader::attachFragmentObject` in `llshadermgr.cpp`/`llglslshader.cpp`): **GLSL does not support referencing a uniform by name across separately-compiled shader objects, even when both objects are attached to and linked into the same program.** `shadowUtil.glsl` is compiled once into its own `GLuint` object (`llviewershadermgr.cpp:916`) and attached to any program with `mFeatures.hasShadows = true` (`llshadermgr.cpp:242-248`) via `glAttachShader` - but that only makes its *functions* callable from other attached objects (via forward declaration, exactly how `sunLightF.glsl` calls `sampleDirectionalShadow()` without ever naming `shadowMap0` itself). It does **not** make its raw `uniform sampler2DShadow shadowMap0..3` declarations visible for direct use in a *different* compiled object. Round 5's `asVolumetricLightF.glsl` violated this by forward-declaring `pcfShadow` (fine, a function) but then also referencing `shadowMap0..3`/`shadow_matrix`/`shadow_clip` directly by name in its own file, where they were never declared - hence "undefined variable" at GLSL link time. This is a genuine constraint of the shader assembly model in this codebase, not a mistake specific to shadow cascade math.

**Structural fix, round 7:** moved the single-cascade sampling logic into `shadowUtil.glsl` itself (tagged `<AS:Chanayane>` addition), as a new function `sampleDirectionalShadowSingleCascade()` where `shadowMap0..3`/`shadow_matrix`/`shadow_clip`/`pcfShadow` are actually in scope. This fixed the link error - the shader compiled and linked cleanly, confirmed via `AyaneStorm.log` showing no errors and `diff` confirming the staged build output matched source.

- Round 8 (round 7's fix, confirmed actually running via log + diff this time): **same symptom as round 4 - mode 3 solid black, mode 2 all white, mode 0/1 flooded white.** With the link failure and stale-build confounders eliminated, this means the custom single-cascade selector's cascade math itself is producing near-zero visibility almost everywhere, despite the index mapping being re-derived and confirmed correct multiple times (independently against `sampleDirectionalShadow()`'s branches and against `pipeline.cpp`'s matrix/clip-plane construction). At this point every individually-inspectable piece (index mapping, `shadow_matrix` multiply, perspective divide/bias order inside `pcfShadow`, `GL_TEXTURE_COMPARE_FUNC`=`GL_LEQUAL`, `GL_CLAMP_TO_EDGE` wrap mode) checked out under static review, yet the combined result is still wrong - strongly suggesting either a subtle interaction between them, or that raymarch sample points (scattered across the view cone via `ray_dir * t`, not confined to the camera's forward axis like a typical shadow receiver) are landing outside each selected cascade's actual rendered shadow-map footprint more often than the distance-only cascade test accounts for, reading arbitrary clamped edge-texel depth as "occluded."
- A parallel investigation into Black Dragon (a sibling Firestorm-derived viewer reported to have working volumetric lighting) found **their implementation is dead code**: `LLPipeline::renderVolumetric()` is entirely commented out, its one call site is also commented out, and their god-ray shader calls a shadow-sampling function (`nonpcfShadowAtPos`) that is forward-declared but never defined anywhere in their tree - it could never have linked. Not usable as a reference.

**Current approach (round 9, not yet rebuilt/tested): abandoned the custom single-cascade selector entirely.** Per user decision, rather than keep debugging a reimplementation against diminishing returns, `asVolumetricLightF.glsl` now calls `sampleDirectionalShadow()` directly - the same real, proven-working multi-cascade function used by every other shadow-consuming shader in this codebase (`sunLightF.glsl`, `alphaF.glsl`, `materialF.glsl`, `waterF.glsl`, etc.), passing `norm = light_dir` (making its surface-bias term a no-op) and keeping the NaN guard from round 4 as a safety net for degenerate blend weights. The `shadowUtil.glsl` addition from round 7 was reverted (file restored to its original, untouched state) since it's no longer called from anywhere. This trades "fully understand and control the cascade selection" for "reuse code with a much larger existing proof of correctness."

**If resuming:** rebuild (`asVolumetricLightF.glsl` changed, `shadowUtil.glsl` reverted to stock - `.glsl`-only, no `.cpp`/`.h` changes). Check `AyaneStorm.log` for link errors first (should be clean - this now matches the exact call pattern of already-working shaders), then test debug mode 3 (raw `min_visibility`). If this **still** reads as solid black, the bug is not in cascade selection/matrix math at all (since this is now byte-for-byte the same shadow function every other working shader uses) and is almost certainly in something specific to this pass's setup - e.g. `sample_pos` not actually being in the view-space that `sampleDirectionalShadow` expects (double check `getPosition()`'s output space against what `bindDeferredShader`'s `inv_proj`/uniforms assume for this specific shader's `mFeatures` configuration), or a texture unit / sampler binding collision from having two custom programs (`gASVolumetricLightProgram`, `gASVolumetricCompositeProgram`) that might be interfering with `LLGLSLShader::mCanBindFast` state or `bindDeferredShader`'s channel allocation in some way specific to this being a newly-added shader outside the normal registration table. That would be the next thing to investigate with fresh eyes rather than continued math re-derivation, since the math has now been checked against a proven-identical reference call and the symptom needs to be explained some other way if it persists.

**Lesson for future `.glsl` debugging in this codebase:** when a shader appears to have no effect or produces suspicious uniform results, check `AyaneStorm.log` (or the platform-equivalent log location) for `LLGLSLShader::createShader : Failed to link` / `dumpObjectLog` entries FIRST. A failed link is caught gracefully by this codebase's `loadShaders()` pattern (feature silently disables) rather than crashing, so a broken shader and a "shader compiles but has wrong math" bug look identical from visual output alone. Also verify the build's staged `app_settings` directory actually matches the source tree before concluding a `.glsl`-only change had no effect - the build/copy step for shader files is separate from the shader's own reload mechanism and can silently lag behind source edits.

If resuming: rebuild (this round changed only `asVolumetricLightF.glsl` - no `.cpp`/`.h`/settings/XUI changes). Retest with `RenderVolumetricLightingDebug = 3` first (raw `min_visibility`) - it should now show mostly white/lit with dark patches only near real occluders, the inverse of the round-4 result. If that looks right, check mode 2 (occlusion) and then mode 0 (normal) in turn. If mode 3 is still solid black or otherwise wrong, the cascade-mapping theory was incomplete or wrong somewhere else (e.g. `shadow_matrix[i]` reprojection itself, or `pcfShadow`'s `shadow_bias`/`shadow_res` globals not being what's expected for this indirect call pattern) and is worth capturing with RenderDoc/Nsight rather than continuing to guess from source reading alone.

- Round 10 (round 9's fix retested): **"Unsurprisingly nothing changed"** - mode 3 still solid black, mode 2 still all white, mode 0/1 still flooded white, even though the shadow-sampling call is now byte-for-byte identical to every proven-working caller. Confirmed via `diff` and log-check (no link errors) that this was a genuine, current, correctly-linked build - ruling out staleness/compile failure yet again. Added debug modes **4** (`abs(ray_end)/64` as RGB - reconstructed view-space position, computed *before* any shadow logic runs) and **5** (`ray_len/128` grayscale) to isolate `getPosition()`/depth reconstruction from shadow sampling entirely. User confirmed the test camera was pointed at **close-up geometry** (avatar, a few meters away), not sky, ruling out "far geometry legitimately reads as white" as an explanation.
  - **Result:** mode 5 read as **uniformly white** (`ray_len` pegged at the 128m cap everywhere, including a few meters from the camera - definitively wrong). Mode 4 showed a **cross pattern**: a vertical cyan band and horizontal magenta band crossing at screen center, avatar visible as a darker silhouette. Decoded as RGB=`abs(ray_end)/64`: low R (cyan has no red) along the vertical centerline, low G (magenta has no green) along the horizontal centerline, high B almost everywhere. That is the signature of `ray_end.x`/`ray_end.y` behaving like raw **NDC/screen coordinates** (zero at screen center, growing toward the edges) rather than real view-space world positions, and `ray_end.z` sitting near a constant - i.e. **`pos.xyz` from `getPosition()` looks like `inv_proj` acted as identity/near-identity** on the `(sc.x, sc.y, 2*depth-1, 1)` NDC input, passing `sc.xy` through mostly unchanged instead of unprojecting it. This directly explains both symptoms: mode 4's "structured but wrong" cross, and mode 5's "always-128" (since `length(ndc.xyz)` for on-screen NDC saturates the `min(..., MAX_MARCH_DISTANCE)` clamp almost everywhere once you're off the near-zero center point).
  - **Investigation into whether `inv_proj` is actually being set for this shader at all** (traced rather than guessed): `inv_proj` is pushed to a shader inside `LLRender::syncMatrices()` (`llrender.cpp:1004`, guarded by a per-shader `mMatHash[MM_PROJECTION]` comparison at `:1094` so it only re-uploads when the projection actually changed since that shader's last sync) - and `syncMatrices()` is called unconditionally from `LLVertexBuffer::drawArrays()` (`llvertexbuffer.cpp:805/922/948`) on every draw, **not** gated by any shader-specific opt-in list. `mMatHash` is initialized to `0xFFFFFFFF` in the `LLGLSLShader` constructor (`llglslshader.cpp:418`), guaranteed to differ from any real hash on first use. So **`inv_proj` should sync correctly for `gASVolumetricLightProgram` even though it's a standalone shader outside `LLViewerShaderMgr::mShaderList`** - this theory, while plausible from the symptom, is not yet confirmed and needs a direct runtime read of `inv_proj`'s actual value for this specific shader, not another round of transitively-reasoned static tracing.
  - **A second, definitely-real, but likely-unrelated bug found along the way:** `LLViewerShaderMgr::finalizeShaderList()` (`llviewershadermgr.cpp:422-`) explicitly enumerates which global `LLGLSLShader*` instances get `mUniformsDirty = true` set every environment update (`llenvironment.cpp:1788`, iterating `LLViewerShaderMgr::beginShaders()/endShaders()` = `mShaderList`). `mUniformsDirty` gates `LLEnvironment::updateShaderUniforms()` (sky/water WindLight uniforms - `sun_dir`, `moon_dir`, `sunlight_color`, etc.), called from `LLGLSLShader::bind()` (`llglslshader.cpp:1086-1090`) only `if (mUniformsDirty)`. **`gASVolumetricLightProgram`/`gASVolumetricCompositeProgram` are never added to `mShaderList`** (deliberately, matching `FSExactOIT`/`FSAVBOIT`'s pattern of owning their shader objects outside the manager's tracked globals) - so `mUniformsDirty` stays at its constructed default of `false` forever, and `sun_dir`/`moon_dir`/`sunlight_color` are **never populated for this shader, not even once**. This is real and needs fixing regardless of the position bug (either register both shaders in `mShaderList`, or have `renderPass()` push these uniforms manually every frame the way it already does for `sample_count`/`scatter_intensity`/etc.) - but it does **not** explain the mode 4/5 symptom, since those debug branches return before `sun_dir`/`sunlight_color` are ever read.
  - **Added debug modes 6 and 7** (not yet tested) specifically to settle the `inv_proj` question with a runtime data point instead of more static tracing: mode 6 outputs `getDepth(pos_screen)` directly (the raw depth-buffer read, before any `inv_proj` involvement at all - isolates whether the *depth buffer sample itself* is sane, independent of unprojection). Mode 7 outputs `getPositionWithNDC(vec3(0,0,0))` (a **fixed** NDC test point, independent of `pos_screen`/depth entirely) as RGB - if `inv_proj` were identity/zero for this shader, mode 7 would read as a **flat, uniform color across the entire screen** (since its input doesn't vary per-pixel), which would prove the `inv_proj`-not-synced theory conclusively; if mode 7 instead shows a plausible fixed view-space point (matching where screen-center/mid-depth should be), `inv_proj` is fine and the bug is specifically in `getDepth(pos_screen)`/`getScreenCoordinate(pos_screen)`'s handling of the real per-pixel `pos_screen`, which mode 6 would then confirm by showing a flat/wrong depth value across the screen despite real geometry being present.

**If resuming:** rebuild (`asVolumetricLightF.glsl` and `panel_preferences_ayanestorm.xml` changed - spinner `max_val` now 7). Test **mode 7 first** - flat color = `inv_proj` not synced for this shader (fix: register `gASVolumetricLightProgram`/`gASVolumetricCompositeProgram` in `LLViewerShaderMgr::mShaderList`, or manually push `INVERSE_PROJECTION_MATRIX` in `renderPass()`); varying-but-plausible color = `inv_proj` is fine, move to mode 6 (flat/wrong depth = `depthMap` binding or `pos_screen`/`getScreenCoordinate` is the real bug; varying/plausible depth = the bug is somewhere between a correct depth read and `getPosition()`'s final `pos /= pos.w` divide, worth re-checking `pos.w` specifically e.g. via a mode 8 that dumps `pos.w` before the divide). Also worth fixing regardless of the outcome: register both volumetric shaders in `mShaderList` (or push WindLight uniforms manually) so `sun_dir`/`moon_dir`/`sunlight_color` stop being silently zero - this doesn't explain modes 4/5 but is a real, separate bug in the normal (mode 0) rendering path once position reconstruction is fixed.

- Round 11 (mode 6/7 tested): **mode 7 (fixed NDC point through `inv_proj`) came back a non-uniform-but-roughly-flat dark blue** - some low-frequency noise/gradient across the screen (plausibly compression/encoding artifacts on a screenshot of a technically-constant value, or genuine slight variation), but critically **not** the stark, structured cross pattern from mode 4, and not an obviously-wrong color like solid black/white. This rules out "`inv_proj` is identity/zero for this shader" - `inv_proj` is a real, working matrix. **Mode 6 (raw `getDepth(pos_screen)`, called directly, no matrix math at all) came back solid white** - confirming the raw depth-buffer *sample itself* reads as far-plane (`1.0`) at literally every pixel, including a few meters from the camera (user confirmed the test target was close-up geometry, not sky - see Round 10's question/answer). Mode 3 also came back solid black (unchanged), mode 2 solid white (unchanged), mode 1 shows the normal lit scene with no god-ray effect visible (unchanged) - consistent with everything downstream of a broken depth read being equally broken.
  - **Follow-up runtime diagnostic (not just static reading this time):** added a one-time `LL_WARNS()` log call in `ASVolumetricLighting::renderPass()` right after `pipeline.bindDeferredShader(gASVolumetricLightProgram)`, reading back `gASVolumetricLightProgram.getUniformLocation(LLShaderMgr::DEFERRED_DEPTH)` and `gASVolumetricLightProgram.mTexture[LLShaderMgr::DEFERRED_DEPTH]` (the actual GL texture-unit channel `depthMap` got mapped to at shader-creation time - `mTexture` is `LLGLSLShader`'s public per-uniform texture-channel lookup table, `llglslshader.h:309`). Rebuilt, retested, checked `AyaneStorm.log` directly. **Result: `uniform_loc=1 texture_channel=0`.** This is a real, valid, non-`-1` channel - `depthMap` is NOT unbound/optimized-out, and `bindDeferredShader`'s `DEFERRED_DEPTH` handling (`pipeline.cpp:9240-9252`, binds `mRT->deferredScreen`'s real depth texture via `gGL.getTexUnit(channel)->bind(deferred_target, true)` with `bindDepth=true`) is executing as designed.
  - **State at end of Round 11, definitively ruled out via direct evidence (not just static review) for this specific shader/pass:** `inv_proj` being unset/identity (mode 7); `depthMap`'s uniform being optimized out or its texture channel being `-1`/unbound (runtime log, `texture_channel=0`); the vertex-shader UV convention (byte-identical to `softenLightV.glsl`/`postDeferredV.glsl`); the full-screen-triangle geometry itself (identical `mScreenTriangleVB` used by working passes); shader attachment order (`hasShadows`/`isDeferred` flags correctly pull in `shadowUtil.glsl`/`deferredUtil.glsl`); GLSL cross-object linking (no link errors, `sampleDirectionalShadow` calls compile and link cleanly since round 9).
  - **What remains unexplained:** a texture IS bound to a valid channel (0), yet sampling through it via `texture(depthMap, pos_screen)` returns `1.0` everywhere, even where mode 4's position visualization (which internally depends on this exact same `getDepth()` call, just further along the same math) showed non-degenerate, spatially-varying output. This apparent contradiction (mode 4 "looks textured", mode 6 says the underlying depth read is flat) was not resolved - it's possible mode 4's cross pattern is *itself* an artifact of a constant depth combined with `sc.xy` (NDC x/y, which DOES vary per-pixel) dominating the `inv_proj` multiply, meaning mode 4 was never actually showing "real-looking" data, just structured-looking data - worth re-examining with fresh eyes rather than assuming mode 4 "passed."
  - **This is the point where static source-reading was exhausted.** Every individually-checkable piece of the chain has now been verified correct in isolation, multiple times, across many rounds (5 through 11), yet the aggregate result stays wrong. Continuing to read code and add debug modes has had rapidly diminishing returns since approximately round 8. **The recommended next step is a GPU capture (RenderDoc or NSight Graphics)** of the actual `AS Volumetric Light Shader` draw call, to directly inspect: (a) what texture object is really bound to unit 0 at draw time, (b) that texture's actual pixel content around the test area, and (c) the real runtime value of `depthMap`/`inv_proj`/`pos_screen` for a representative fragment via the capture tool's shader debugger - this replaces guessing with ground truth. A lower-effort alternative worth trying first: bypass `bindDeferredShader`'s generic binding for `depthMap` specifically and bind it manually in `renderPass()` (`gGL.getTexUnit(N)->bind(&mRT->deferredScreen, true)`, matching the manual-bind pattern already used elsewhere for the same target at `pipeline.cpp:8453`/`:8866`/`:9107`) to see if that changes the result - if it does, something about going through the generic multi-texture `bindDeferredShader` path specifically (channel allocation order, a subsequent `enableTexture` call for a different uniform silently reusing/overwriting channel 0, etc.) is the actual bug, narrowing the search significantly.

### Post-build fixes (found via in-game testing and code re-inspection, not the original code review)

1. **Raw-meters distance multiply.** `scatter *= ... * ray_len` used `ray_len` (up to `MAX_MARCH_DISTANCE = 128`) as a raw multiplier against an already ~[0,1]-normalized scatter value, blowing the whole frame to solid white. Fixed by normalizing to `distance_factor = ray_len / MAX_MARCH_DISTANCE` (0..1) instead.
2. **Averaging visibility instead of measuring occlusion.** The raymarch averaged `sampleDirectionalShadow()` (visibility, ~1.0 on unoccluded/open-sky rays) across all samples. Since most of a typical view has visibility ≈1 along the *entire* ray, this produced a large, nearly uniform "scatter" value across almost the whole screen — a broad wash, not spatially-varying light shafts. Fixed by tracking `min_visibility` along the ray and using `occlusion = 1.0 - min_visibility`: rays that never pass through shadow now contribute ~0.
3. **Unmanaged GL depth-test/blend state.** `renderPass()`'s two full-screen-triangle draws never explicitly set depth-test or blend state; they inherited whatever `renderFinalize()`'s caller left active, which is unpredictable (the real `gDeferredSunProgram` draw, by contrast, always wraps itself in explicit `LLGLDepthTest`/`LLGLDisable(GL_BLEND)` guards, per `pipeline.cpp` around `:9493-9496`). Fixed by adding `LLGLDepthTest depth(GL_FALSE, GL_FALSE)` for the whole pass plus explicit per-draw blend guards.
4. **Debug mode added.** `RenderVolumetricLightingDebug` (S32, non-persistent, temporary - remove once the effect works): `0` normal, `1` composite pass replaces `screen` outright with the raw upsampled scatter (color+intensity applied, bypasses tonemap), `2` raymarch shader itself outputs raw grayscale `occlusion` and skips all color/phase/intensity/distance weighting. Exposed as a spinner in the AyaneStorm preferences panel (`panel_preferences_ayanestorm.xml`) directly under the volumetric lighting checkbox, enabled only when that checkbox is on. **Remove this control, the setting, and the `debug_mode` uniform/branch once the effect is confirmed correct** - it was requested as a temporary diagnostic aid, not a permanent feature.

### Preferences tab decision (user-directed deviation from the original plan)

The user flagged that `ASPanelPrefsAyaneStorm::postBuild()` was hiding the entire `tab-as-rendering` sub-tab on `LL_DARWIN`, because until now it only held GL 4.3-only Exact OIT/AVBOIT settings (`RenderOITMode`). Since volumetric lighting is designed to also run on macOS (GL 4.0/4.1 floor), putting its checkbox in that tab as originally planned would have made it invisible on Mac. Resolved by user choice: **stop hiding the whole tab on Darwin; instead hide only the `render_oit_mode`/`render_oit_mode_label` controls that are actually GL-4.3-specific**, leaving the tab (and the new volumetric checkbox) visible on Mac. See `aspanelprefsayanestorm.cpp` `postBuild()`.

### Refinements discovered during implementation (post-planning; corrections to the plan below)

- **Shader template is simpler than originally planned.** `sunLightF.glsl`/`sunLightV.glsl` (the real `gDeferredSunProgram`, registered at `llviewershadermgr.cpp:1749-1762`) is the ideal, minimal template: the fragment shader just forward-declares `vec4 getPosition(vec2)`, `vec4 getNorm(vec2)`, and `float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen)` — no manual depth-reconstruction math and no `#include`, no manual permutation (`addPermutation("HAS_SUN_SHADOW", ...)` is NOT needed here — setting `mFeatures.hasShadows = true` on the `LLGLSLShader` is sufficient; that's a `softenLightF.glsl`-specific pattern, not universal). Linking `shadowUtil.glsl`/`deferredUtil.glsl` happens automatically based on `mFeatures`.
- **`norm` for a raymarch sample.** `sampleDirectionalShadow(pos, norm, pos_screen)` uses `norm` only to bias the shadow lookup away from surface peter-panning — irrelevant for a mid-air volumetric sample. Passing a fixed `vec3(0,0,1)` (or the light direction itself) as `norm` at each raymarch step is fine; it only affects the bias offset, not correctness.
- **Composite binding is simpler than raw `bindDeferredShader`.** `LLGLSLShader::bindTexture(channel, target)` (used by `combineGlow`, `pipeline.cpp:8454-8473`) is the right API for the composite pass — bind `mRT->screen` and `sVolumetricTarget` as two textures and additive-blend, no need for `enableTexture`/`getTexUnit` plumbing (that's what `bindDeferredShader` does internally for the *raymarch* pass, which does need the full G-buffer + shadow maps).
- **`mScreenTriangleVB` is public** on `LLPipeline` (`pipeline.h:801`, between the `public:` at `:514` and `protected:` at `:864`) — `renderPass(LLPipeline&, ...)` can use `pipeline.mScreenTriangleVB` directly, as planned.
- **Do NOT compile out on `LL_DARWIN`.** `FSExactOIT`/`FSAVBOIT` both `#if LL_DARWIN` stub themselves out entirely (`fsexactoit.cpp:38-99`) because they hard-require GL ≥4.3. `ASVolumetricLighting` is designed to work at GL 4.0/4.1, i.e. specifically to run on Mac — it must NOT get the same `#if LL_DARWIN` treatment, or the whole point of the GL-4.1-compatible design is lost.
- **`bindDeferredShader()` does NOT set `sun_up_factor`.** Unlike `sun_dir`/`moon_dir`/`sunlight_color`/`moonlight_color` (all set unconditionally inside `bindDeferredShader`, `pipeline.cpp:9389-9418`), `SUN_UP_FACTOR` is set by *callers* of the deferred lighting shaders (e.g. `renderDeferredLighting()`'s `soften_shader.uniform1i(LLShaderMgr::SUN_UP_FACTOR, ...)` at `:9604`), not by `bindDeferredShader` itself. Since `ASVolumetricLighting::renderPass()` calls `bindDeferredShader()` directly rather than going through `renderDeferredLighting()`, it must set `sun_up_factor` itself via `LLEnvironment::instance().getIsSunUp()` — done in `asvolumetriclighting.cpp`.
- **Unbounded ray length for sky pixels.** `getPosition()` returns the far-clip distance (effectively huge) for pixels with no geometry (sky/horizon). The raymarch fragment shader clamps `ray_len` to a constant `MAX_MARCH_DISTANCE = 128.0` to avoid pathologically long marches and scatter intensity blowing out on sky pixels (`asVolumetricLightF.glsl`).
- **Composite must not read `screen` while writing to it.** The composite pass renders into `mRT->screen` using additive GL blending (`BT_ADD`); it must only sample the low-res volumetric target (`emissiveRect`/`DEFERRED_EMISSIVE`), not also bind `screen` itself as a source texture (which would be simultaneous read/write of the same attachment — undefined behavior). `asvolumetriclighting.cpp`'s `renderPass()` was corrected to drop the `DEFERRED_DIFFUSE` bind of `screen` that an earlier draft had.

## Context

AyaneStorm (Firestorm/SL viewer fork) has no volumetric/god-ray lighting today. This document plans adding it as an *optional* feature, following `AGENTS.md`'s conventions:
- New files are `as`-prefixed and need no upstream ownership tags, but do need file-header comments.
- Any edit to an `ll*`/`fs*` file must be wrapped in `// <AS:Chanayane> ... // </AS:Chanayane>` tags, with original code kept commented inside the tags when replacing it.
- "Substantial functionality must be placed in a new module instead of enlarging an existing upstream or shared module" — `AGENTS.md` names `fsexactoit` as the model. That module **exists** in this tree (`indra/newview/fsexactoit.h/.cpp`, see also `doc/ayanestorm-special-exact-oit-how-it-works.md`) and is the closest real precedent: it offloads an entire rendering feature out of `pipeline.cpp`/`llviewershadermgr.cpp` behind a static-method API (`isEnabled()`, `loadShaders()`, `allocateResources()`, `releaseResources()`, `finishFrame()`), and `pipeline.cpp` only contains small tagged call-outs, e.g.:
  ```cpp
  // <AS:Chanayane> Allocate Exact OIT resources for the main full-resolution target.
  FSExactOIT::allocateResources(resX, resY);
  // </AS:Chanayane>
  ```

This plan follows that exact shape, with the offload module named `ASVolumetricLighting` (AS-prefixed since it's new, per convention) rather than an `fs`-prefixed name, since it isn't Firestorm-upstream functionality.

## Existing prerequisites (verified in codebase)

- Deferred renderer with full G-buffer (`LLPipeline`, `indra/newview/pipeline.cpp`).
- Cascaded sun shadow maps generated every frame: `LLPipeline::generateSunShadow` (`pipeline.cpp:10879`).
- Shadow-sampling shader helper already used by lighting shaders: `sampleDirectionalShadow(pos, norm, pos_screen)` in `app_settings/shaders/class1/deferred/shadowUtil.glsl:96`.
- Frame order (verified by reading the call sites, not inferred):
  - `generateSunShadow` → `renderGeomDeferred` → `renderDeferredLighting()` (`pipeline.cpp:9424`) — the **main-frame** call is `llviewerdisplay.cpp:1174`, inside `display()`.
  - Later, inside `render_ui()`, `LLPipeline::renderFinalize()` (`pipeline.cpp:8941`, called from `llviewerdisplay.cpp:1646` under the comment "apply gamma correction and post effects") does tonemap/gammaCorrect (`:8962-8986`) then `generateGlow`/`combineGlow` (`:8990-8995`).
  - `renderFinalize()` takes `mRT->screen` as its **source** (`:8975`, `:8985`), so anything additively composited into `mRT->screen` before it runs is picked up by tonemap/exposure automatically.
  - Note: `llviewerdisplay.cpp:1395` is *not* a second main-frame site — it's the reflection/cube-snapshot path, where `renderFinalize()` is explicitly commented out (`:1400`). `renderFinalize()` also asserts `!gCubeSnapshot` (`:8943`), so the volumetric pass is excluded from cube snapshots/reflection probes.
- Existing glow/bloom pass (`generateGlow`, `pipeline.cpp:8002`; `combineGlow`, `:8454-8473`) is the template for a full-screen accumulate-then-composite pass.
- Settings pattern: `RenderShadowDetail` (`settings.xml:13179-13189`) and quality-preset lines in `featuretable*.txt`.
- Offload precedent: `FSExactOIT` (`fsexactoit.h/.cpp`) and `FSAVBOIT` (`fsavboit.h/.cpp`), both driven from tagged call-outs in `pipeline.cpp`/`llviewershadermgr.cpp`, both already registered in `indra/newview/CMakeLists.txt` alongside `asstreamkeeper.cpp` (`:98`, `:122`).

## macOS / OpenGL 4.1 compatibility (researched)

**Yes, this feature works on GL 4.1 core, with one hard constraint: no compute shaders or SSBOs.**

- GLSL `#version` is injected at compile time by `LLShaderMgr::loadShaderFile` (`indra/llrender/llshadermgr.cpp:569-604`) from the runtime-detected driver version — no `.glsl` file hardcodes a `#version` line, so no macOS-specific shader fork is needed. **On a Mac at GL 4.1 / GLSL 4.10**, `major_version >= 4` but `minor_version < 20`, so the shader compiles as `#version 400` (`:603`). Everything the raymarch needs (`sampler2DShadow`, `texture()`, dynamic `for` loops) is core in GLSL 4.00 — but the pass must not use any 4.2+/4.3+ syntax.
- The existing `<AS:Chanayane>` block at `llshadermgr.cpp:572-597` special-cases OIT/compute shaders to receive `#version 430`, matched by filename (`exactOIT`, `avboit`) or defines. **The volumetric shaders must NOT be added to that special case** — staying out of it is what keeps them compiling at 400 on Mac.
- Existing class2/class3 deferred shaders already use `for` loops with dependent shadow-map `texture()` fetches (`reflectionProbeF.glsl`, `multiPointLightF.glsl`, `blurLightF.glsl`) — a ~16-24 step raymarch is well within what already ships; no unrolling/branching restriction.
- **Trap to avoid**: `FSAVBOIT` hard-requires GL ≥4.3 (compute + SSBOs), gated by `FSAVBOIT::supported()` (`fsavboit.cpp:379-384`). That path silently disables on Mac's 4.1 ceiling. The volumetric pass must **not** follow it — keep it a plain fragment-shader raymarch.
- Caveat: this codebase has **no live `__APPLE__`/Mac-specific gating anywhere in `indra/newview`** — Mac support appears untested in this fork generally, so "compatible per GL 4.1 spec" is unverified in practice until built and run on macOS hardware.

## Architecture: `ASVolumetricLighting`

New module modeled directly on `FSExactOIT`'s shape:

**`indra/newview/asvolumetriclighting.h`**
```cpp
class ASVolumetricLighting
{
public:
    static const char* shaderCacheRevision();  // bump on any .glsl edit; see note below
    static bool isSupported();      // GL/GLSL floor check, mirrors FSAVBOIT::supported()
    static bool isEnabled();        // settings + isSupported() + RenderShadowDetail > 0
    static bool loadShaders(S32 shader_level);
    static void unloadShaders();
    static void allocateResources(U32 width, U32 height);
    static void releaseResources();
    static void renderPass(LLPipeline& pipeline, LLRenderTarget& screen);  // raymarch + composite

    static S32 getSampleCount();
    static F32 getScatterIntensity();
    static F32 getScatterAsymmetry();

private:
    static bool sSupportChecked;
    static bool sSupported;
    static LLRenderTarget sVolumetricTarget;
};
```

The `.cpp` owns: settings reads, the GL-version support check, quality-based sample-count scaling, shader load/unload, resource alloc/release, and the draw calls. `LLPipeline`/`LLViewerShaderMgr` never contain the "why," only tagged one-line call-outs — same division of responsibility as `FSExactOIT`.

Two details from the existing OIT modules that are easy to miss:

- **`isSupported()`**: mirror `FSAVBOIT::supported()`'s shape (`fsavboit.cpp:379-384`) but with a **GL 4.0 / GLSL 4.00 floor**, not AVBOIT's 4.3 — the point is that this feature stays available where AVBOIT is not (macOS at 4.1). Cache in `sSupported`/`sSupportChecked`.
- **`shaderCacheRevision()`**: both OIT modules expose one, and `llviewershadermgr.cpp:563-564` folds them into the shader-cache hash (`hash_obj.update(FSExactOIT::shaderCacheRevision())`). Without an equivalent, edited `.glsl` files can be served stale from cache during development. Add a matching tagged `hash_obj.update(ASVolumetricLighting::shaderCacheRevision())` there, and bump the string on every shader edit. Both modules also ship an "unsupported" stub of this function (`fsavboit.cpp:23`, `fsexactoit.cpp:51`).

## Implementation steps

### 1. `ASVolumetricLighting` module (new, no ownership tags needed)
Create `asvolumetriclighting.h/.cpp` with the shape above. File header modeled on `fsexactoit.h`'s but attributed as an AyaneStorm-original module (`@author chanayane@firestorm`, `$LicenseInfo:...AyaneStorm...$` block matching `asstreamkeeper.h`).

### 2. Settings
Add `RenderVolumetricLighting` (Boolean, default `0`) to `app_settings/settings.xml`, copying the `RenderShadowDetail` map structure (`settings.xml:13179-13189`).

Wrap it in **XML-comment-form** ownership tags, matching the existing Exact OIT setting blocks at `settings.xml:11811-11825` / `:11826-11840` — i.e. `<!-- <AS:Chanayane> Volumetric lighting. -->` … `<!-- </AS:Chanayane> -->`, *not* C++ `//` comments.

### 3. Render target + resource lifecycle
`allocateResources(width, height)`/`releaseResources()` own `sVolumetricTarget` entirely. The module halves the passed dimensions internally (raymarching at full res is the main perf risk; half-res is the usual starting point, with the bilateral upsample hiding it), so the call site passes the same `resX, resY` that `FSExactOIT::allocateResources` receives without knowing the divisor.

`pipeline.cpp` gets two tagged call-outs mirroring the `FSExactOIT` alloc/release call sites (near `pipeline.cpp:1013-1015` and `:1422-1425`):
```cpp
// <AS:Chanayane> Allocate volumetric lighting resources alongside Exact OIT.
ASVolumetricLighting::allocateResources(resX, resY);
// </AS:Chanayane>
```

### 4. Shaders (new files, `as`-prefixed, no ownership tags needed)
Plain fragment-shader raymarch — **no compute shaders, no SSBOs**. Under `app_settings/shaders/class2/deferred/` (required location for `LLViewerShaderMgr` to find them):
- `asVolumetricLightV.glsl` — full-screen triangle vertex shader (copy `glowV.glsl`).
- `asVolumetricLightF.glsl` — reconstructs view-space position from `depthMap` (reuse `deferredUtil.glsl` helpers), loops N times (N from `getSampleCount()`), calls `sampleDirectionalShadow()` per step, accumulates scatter modulated by sun color + `scatterAsymmetry`/`scatterIntensity` uniforms.
- `asVolumetricCompositeF.glsl` — bilateral-upsample + additive composite, modeled on `combineGlow` and `glowcombineF.glsl`'s sampler-naming convention (`diffuseRect`/`depthMap`-style names).

### 5. Shader registration (`llviewershadermgr.cpp` — tagged call-outs only)
`loadShaders(shader_level)` does the actual `LLGLSLShader` construction/`createShader()` call, mirroring `gGlowProgram`'s load block (`llviewershadermgr.cpp:1034-1048`) and `gGlowCombineProgram`'s post-load uniform binding (`:3279-3290`) — but the `LLGLSLShader` instance and its load logic live inside `asvolumetriclighting.cpp`, exactly as `FSExactOIT` owns its own shader objects rather than adding them to the global shader arrays.

Call site: the end of `loadShadersDeferred()`, in the existing `<AS:Chanayane>` block at `llviewershadermgr.cpp:3068-3074`:
```cpp
// <AS:Chanayane> Load AVBOIT from vanilla shaders, then load Exact OIT independently.
    if (success) { FSAVBOIT::loadShaders(mShaderLevel[SHADER_DEFERRED]); }
    success = FSExactOIT::loadShaders(success, mShaderLevel[SHADER_DEFERRED], use_sun_shadow, ...);
// </AS:Chanayane>
```
Add the volumetric load inside/adjacent to that block. Follow **`FSAVBOIT`'s** shape rather than `FSExactOIT`'s: it does *not* fold its result into `success`, so a volumetric shader-compile failure disables only this optional feature instead of failing the whole deferred shader load. Add the matching `unloadShaders()` call-out wherever `FSExactOIT::unloadShaders()` is called.

### 6. Pipeline integration (`pipeline.cpp` only — one tagged call-out)
`renderPass(pipeline, screen)` contains the entire bind/raymarch/composite sequence, early-returning internally via `isEnabled()`.

**Call site: the top of `LLPipeline::renderFinalize()` (`pipeline.cpp:8941`), immediately before the HDR/tonemap block at `:8962`.** Preferable to hooking `llviewerdisplay.cpp` after `renderDeferredLighting()` because:
- It composites into `mRT->screen` right before `renderFinalize()` consumes that same target as its tonemap source (`:8975`/`:8985`), so god-ray scatter goes through the same HDR exposure/tonemap as the rest of the scene — unlike glow, which composites *post*-tonemap.
- `renderFinalize()` already asserts `!gCubeSnapshot` (`:8943`), so the cube-snapshot/reflection-probe path is excluded for free, and `llviewerdisplay.cpp` is left untouched (one fewer upstream file to tag and re-merge).

`renderPass()` should still guard internally on `gCubeSnapshot`/`sRenderingHUDs` defensively, matching how `FSExactOIT::finishFrame()` takes `cube_snapshot`/`impostor_render`/`mouselook` flags.

**Texture binding — reuse `LLPipeline::bindDeferredShader()` (`pipeline.cpp:9188`) rather than hand-binding.** This is the biggest implementation shortcut and it validates the design:
- It binds the full G-buffer (`DEFERRED_DIFFUSE`/`SPECULAR`/`NORMAL_MAP`/`EMISSIVE`/`DEFERRED_DEPTH`, `:9196-9236`), the exposure map, viewport uniforms, **and the sun shadow maps via `bindShadowMaps(shader)`** (see `bindDeferredShaderFast`, `:9178`).
- Binding is driven by `shader.enableTexture(...)`, a no-op for samplers the shader doesn't declare — so `asVolumetricLightF.glsl` declares `depthMap` plus the `shadowMap0..3`/`shadow_matrix[]` uniforms from `shadowUtil.glsl` and gets them bound for free.
- Pair with `unbindDeferredShader(shader)` on the way out, as `renderDeferredLighting()` does at `:9482`/`:9498`.

So `renderPass()` is essentially: `bindDeferredShader` → set the scatter uniforms → `mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3)` → `unbindDeferredShader`, mirroring the sun-shader block at `:9481-9498`.

### 7. Graphics quality presets
Add `RenderVolumetricLighting <available> <recommended>` to each `list` block in `featuretable.txt` / `featuretable_mac.txt` / `featuretable_linux.txt`, following the `RenderShadowDetail` line pattern. `recommended=0` everywhere (opt-in); set `available=0` on the lowest tiers.

### 8. Preferences UI
Add a checkbox to `ASPanelPrefsAyaneStorm` (`aspanelprefsayanestorm.cpp` + `skins/default/xui/en/panel_preferences_ayanestorm.xml`) bound to `RenderVolumetricLighting` — AS-owned files, no tagging needed. Disable/gray it when `RenderShadowDetail == 0`, since the effect has no source without shadows.

### 9. Documentation
Per `AGENTS.md`, fold the GL 4.1/macOS findings above into `/doc` (this file, or a dedicated `doc/volumetric-lighting-macos-gl41.md`) so they're reusable without re-deriving.

## Files touched

**New (no ownership tags required, still need file-header comments):**
- `indra/newview/asvolumetriclighting.h`, `.cpp`
- `indra/newview/app_settings/shaders/class2/deferred/asVolumetricLightV.glsl`, `asVolumetricLightF.glsl`, `asVolumetricCompositeF.glsl`

**Existing `ll*`/`fs*` files — every edit wrapped in `<AS:Chanayane>` tags, author `chanayane@firestorm`:**
- `indra/newview/pipeline.cpp` — resource alloc/release call-outs (`:1013-1015`, `:1422-1425`) and the render-pass call-out at the top of `renderFinalize()` (`:8941`, before `:8962`). No `pipeline.h` change needed: the render target lives in `ASVolumetricLighting`, not `LLPipeline`.
- `indra/newview/llviewershadermgr.cpp` — load call-out in the existing AS block at `:3068-3074`, unload call-out, and the shader-cache-hash line at `:563-564`.
- `indra/newview/app_settings/settings.xml` — one new tagged setting entry (XML-comment tags).
- `indra/newview/app_settings/featuretable*.txt` — one tagged line per quality list.
- `indra/newview/CMakeLists.txt` — register the new `.cpp`/`.h` (it already lists `asstreamkeeper.cpp` at `:98` and `fsexactoit.cpp` at `:122`).

**Not touched:** `indra/newview/llviewerdisplay.cpp` — hooking `renderFinalize()` instead of the `display()` call site avoids it entirely.

**AS-owned files, no tags needed:**
- `indra/newview/aspanelprefsayanestorm.cpp` + `skins/default/xui/en/panel_preferences_ayanestorm.xml` — new checkbox.

## Verification

1. Build with `RenderVolumetricLighting` off (default) — confirm no visual/perf change; `isEnabled()` should make `renderPass()` a no-op at effectively zero cost.
2. Enable via the AyaneStorm preferences panel with `RenderShadowDetail >= 1`, strong directional sun angle, occluders present — confirm visible light shafts through gaps.
3. Set `RenderShadowDetail` to 0 while the setting is on — confirm the pass disables cleanly, no crash/shader errors.
4. Profile frame time on/off (viewer stats window or Tracy); tune the default sample count and the half-res divisor accordingly.
5. Test underwater and in edit mode for artifacts at the water plane or in build tools; confirm HUD rendering is unaffected.
6. Test on NVIDIA and AMD/Intel if available. If a Mac build is available, verify `isSupported()` reports correctly and that the plain-fragment-shader design compiles and runs under the GL 4.1 core profile (expect `#version 400`).
