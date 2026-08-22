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

The configurable sun/moon intensity ceiling was later raised from `5.0` to
`8.0` for additional artistic headroom. Its shipped default remains `0.8`.
Sun/moon intensity now also uses two decimal places and `0.01` increments,
matching the local-light control and allowing values such as `0.01` through
direct entry or slider-key adjustment.

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

### Debug modes 6 and 7 display correction

Later testing again reported mode 6 as white and mode 7 as black. Both were
expected consequences of poor diagnostic display scaling rather than evidence
of a new depth failure: perspective device depth clusters extremely close to
1, while the fixed mid-depth inverse-projection position used by mode 7 can be
tiny compared with the arbitrary 64-metre display divisor. Revision `v8`
changes mode 6 to display `clamp((1-depth)*1000)` and mode 7 to display the
absolute normalized reconstructed direction. Mode 7 uses explicit magenta only
if inverse projection returns a zero-length vector. Tooltips were updated to
describe the new encodings.

### Local-light diagnostic modes

Modes 1 through 7 originally did not respond when local lights were toggled
because local accumulation was deliberately skipped for every nonzero debug
mode, preserving the directional diagnostics. This separation is now explicit
rather than implicit. Revision `v9` adds local-only mode 8 (raw local scatter,
including its intensity and normalization) and mode 9 (grayscale fraction of
selected local-light spheres intersecting each visible camera ray). Modes 1
through 7 remain sun/moon-only. Local modes require **Include local lights** to
be enabled; otherwise their correct result is black.

Mode 7's first normalized-direction test rendered uniformly blue. That was a
valid result for the chosen screen-center NDC point, whose reconstructed ray is
almost entirely view-space Z, but it made the diagnostic unnecessarily weak.
Revision `v10` uses fixed off-center NDC `(0.5, 0.25, 0.0)` so a valid inverse
projection exercises X, Y, and Z and should display a uniform mixed RGB color.
Magenta remains the explicit zero-vector failure signal.

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

## Post-fix quality/scope changes (2026-08-20, after the core effect was confirmed working)

With the directional god-ray effect confirmed correct (mode 0 matches expectations against the disabled baseline), work shifted from bug-fixing to quality and scope:

- **Local-light cap raised 32 → 64.** User reported no measurable FPS impact at 32 selected local lights, so `MAX_VOLUMETRIC_LOCAL_LIGHTS` (`asvolumetriclighting.cpp`), the `local_light[]`/`local_light_color[]` uniform array sizes (`asVolumetricLocalLightF.glsl`), and the `render_volumetric_local_lights_max_count` spinner's `max_val` (`panel_preferences_ayanestorm.xml`) were all raised to 64 in lockstep. The persisted default (`RenderVolumetricLocalLightsMaxCount = 8`) was left unchanged — this raises the ceiling, not the out-of-the-box value. `settings.xml`'s comment string was updated to match. Shader cache revision bumped to `v11`.
- **Per-pixel raymarch dithering added.** The fixed sample count (16-24 depending on shadow detail) produced visible stepped/banded rings at shadow transition boundaries. Added `interleavedGradientNoise()` (Jimenez 2014 constants) to `asVolumetricLightF.glsl`, and changed the raymarch loop's per-step distance from a fixed `(float(i) + 0.5) * step_len` to `(float(i) + jitter) * step_len`, where `jitter = interleavedGradientNoise(gl_FragCoord.xy)` (real pixel coordinates, not `vary_fragcoord`'s `[0,1]` UV, so the noise varies per screen pixel). This trades visible banding for fine, much less objectionable grain, which the half-res-to-full-res bilinear upsample in the composite pass further softens. User confirmed this looks good. Shader cache revision bumped to `v12`.
- **Debug mode 7's NaN/zero ambiguity fixed.** User noticed mode 7 read as solid magenta and pushed back on the assumption that this was a harmless stale leftover from the now-resolved depth-timing bug. On inspection, the old code was genuinely ambiguous: `frag_color = fixed_length > 1e-6 ? ... : magenta` conflates two very different situations, since a `NaN` length also fails the `> 1e-6` comparison in GLSL (all comparisons against NaN are false), making magenta an indistinguishable readout for "the vector is truly near-zero" and "the length itself is NaN/Inf". Rewrote mode 7 to check `is_finite = (fixed_length == fixed_length) && (fixed_length < 1e30)` explicitly first (the classic self-inequality NaN test, plus an Inf guard), and only falls through to the near-zero/magnitude-ramp logic when the value is confirmed finite: **magenta now means NaN/Inf specifically**, **black means a real near-zero result**, and a **green ramp** (`log2(length+1)/10`, clamped) encodes any other finite nonzero magnitude. Green is only a catastrophic-result sanity check, not proof that `inv_proj` is otherwise correct. This is a diagnostic-only change (`asVolumetricLightF.glsl`, mode 7 branch and its comment block); it does not touch the mode-0 rendering path. Shader cache revision bumped to `v13`.

**Retested: mode 7 reads green.** This confirms only that the fixed inverse-projection result is finite and nonzero; an identity, stale, or otherwise wrong matrix could also satisfy that test. The original magenta was caused by the old diagnostic conflating non-finite and near-zero outcomes. Confidence in the actual position-reconstruction path instead comes from the working mode-0 result together with the scene-shaped mode-4 and mode-5 results. No position-reconstruction bug is currently evident.

During active development, the user manually clears the shader cache. Keep
`shaderCacheRevision()` stable to avoid unnecessary LTO relinks; bump it before
distributing a build whose users will retain existing shader caches.

- **Depth-aware (bilateral) composite upsampling added.** The composite pass previously did a plain hardware-bilinear sample of the half-res `emissiveRect` scatter target, which bleeds/haloes color across depth discontinuities (silhouette edges, doorframes, foliage) since hardware bilinear has no notion of scene depth. `asVolumetricCompositeF.glsl` now manually reconstructs the 4 bilinear source taps (`uv00/uv10/uv01/uv11`, using a new `emissiveRectDelta` uniform = `1/half-res target resolution`) and weights each by `1/(|depth_tap - depth_center| + DEPTH_EPS)`, where depth comes from `getDepth()` (full-res `depthMap`, from `deferredUtil.glsl`, already available since `gASVolumetricCompositeProgram.mFeatures.isDeferred = true`). `asvolumetriclighting.cpp`'s composite section now explicitly binds `pipeline.mRT->deferredScreen` as `DEFERRED_DEPTH` (same `bindTexture(..., true)` pattern already used for the raymarch pass) and uploads `emissiveRectDelta`. `emissiveRect`'s texture-address mode is now explicitly set to `TAM_CLAMP` on bind, since the 4 manual taps can land fractionally outside `[0,1]` at the screen border and the render target's default wrap mode was not guaranteed to be clamp. `DEPTH_EPS = 0.0005` uses raw nonlinear device depth (not a linearized value) as a cheap approximation - device depth is packed close to `1.0` at scene-typical distances, so this epsilon was chosen empirically rather than derived; **not yet visually verified**, worth revisiting if the bilateral weighting looks too aggressive/soft in practice. Not yet built or tested.

**If resuming:** rebuild (`asVolumetricCompositeF.glsl` and `asvolumetriclighting.cpp` both changed). Test around a high-contrast silhouette edge or doorframe with volumetric lighting enabled at debug mode 0 - color bleed across the edge should be visibly reduced compared to before, without introducing new artifacts (over-sharpening, dark rings, or the effect disappearing entirely near edges, which would indicate `DEPTH_EPS` is too small and weights are collapsing to near-zero everywhere). Per explicit user instruction, `shaderCacheRevision()` is intentionally NOT bumped for this change (still `v13`) - the developer clears the shader cache by hand before each test build; do not "helpfully" bump it, and do not fight this instruction by reverting it back to auto-bump guidance in this doc.

**Correction (verified independently, not just taken on trust):** the original tap-coordinate math above had a real bug, fixed by a later pass. `base_uv = tap_uv - fract(tap_uv/delta)*delta` (equivalent to `floor(tap_uv/delta)*delta`) lands each of the 4 bilinear taps on a texel *edge*, not its *center* - the standard "convert to texel space, floor, add 0.5 back, convert to UV" re-centering step was missing. Numerically and symbolically verified: the corrected version's `base_uv` is exactly `0.5 * emissiveRectDelta` (half a texel) further along in each axis than the original, for every input tested - a systematic, constant offset, not an edge-case rounding difference. The corrected version also clamps `uv00/uv10/uv01/uv11` directly with `clamp(..., min_uv, max_uv)` rather than relying only on `emissiveRect`'s `TAM_CLAMP` address mode, which is a real robustness improvement the original missed: `getDepth()` samples `depthMap`, a *different* texture whose wrap mode was never touched and can't be assumed clamped. Both fixes are correct and are now in `asVolumetricCompositeF.glsl`.

Static review corrected two definite issues before the first test. The manual
bilinear reconstruction now uses the standard texel-center transform
`floor(uv / delta - 0.5) + 0.5`; the earlier base coordinates landed on texel
boundaries and caused every nominal tap to be bilinearly filtered again. All
four UVs are also clamped to valid half-resolution texel centers before both
scatter and full-resolution depth sampling, so correctness no longer depends
on either texture's wrap mode. The experimental raw-device-depth epsilon was
left unchanged pending visual evidence. Per development-stage instruction,
the shader revision remains `v13` because the shader cache is manually cleared.

### Regression found in-game: bright streak across alpha-blended hair (2026-08-20)

User tested the corrected bilateral upsample against an avatar and reported a bright blue-white streak cutting across hair strands at the head/sky silhouette, confirmed as a regression that only appeared after the bilateral upsample change (not present before it).

**Root cause:** hair is alpha-blended geometry and does not write the depth buffer. `getDepth()` at a hair pixel therefore silently returns whatever is *behind* the hair (skin, or sky showing through gaps between strands) rather than the hair itself. The bilateral weight `1.0 / (d + DEPTH_EPS)` used in the previous version is unbounded as `d → 0` - at a strong depth discontinuity (head against sky) it only takes one of the 4 taps landing on a depth value that happens to coincidentally match the center pixel's (background-leaking-through-hair) depth for that tap's weight to blow out and completely dominate the other three, producing a bright, wrongly-composited streak. This is a structural blind spot of comparing against the deferred depth buffer at all - the volumetric pass, like the deferred renderer's depth buffer itself, has no notion of alpha-blended surfaces.

**Fix applied:** replaced the unbounded `1/(d + DEPTH_EPS)` weight with a bounded exponential similarity weight, `max(exp(-d * DEPTH_FALLOFF), MIN_WEIGHT)` with `DEPTH_FALLOFF = 4000.0` and `MIN_WEIGHT = 0.05`. This still favors depth-matching taps (preserving the intended silhouette-bleed reduction for ordinary opaque geometry) but can never let a single tap fully dominate the other three, since every tap keeps at least `MIN_WEIGHT` of its bilinear share regardless of depth mismatch. This trades a small amount of edge sharpness at genuine opaque-geometry discontinuities for not corrupting alpha-blended geometry (hair, foliage, etc.) that the depth buffer is blind to in the first place. `asVolumetricCompositeF.glsl` only; no `.cpp` changes. Not yet retested in-game.

**Retested: the bounded-weight fix did not resolve it.** Two comparison screenshots (before/after the weight-clamping fix) still showed a visible glow/over-brightening along hair strands against a bright window background. The exponential-falloff + minimum-weight change reduced but did not eliminate the artifact.

**Decision: reverted the bilateral upsample entirely, back to plain bilinear.** User explicitly stated alpha/transparency-adjacent behavior (hair, OIT/AVBOIT rendering) must not be touched or risked further, given the amount of separate work already invested in getting that correct elsewhere in this codebase - the bilateral upsample's core premise (comparing against the opaque deferred depth buffer, which is fundamentally blind to alpha-blended geometry) could not be fixed by tuning weight constants, since the depth information it needs simply isn't there for hair. Rather than keep iterating blind against a structurally-limited approach, `asVolumetricCompositeF.glsl` was reverted to a single `texture(emissiveRect, vary_fragcoord)` bilinear sample - functionally identical to the pre-bilateral-upsample version. The now-unused C++ side (`asvolumetriclighting.cpp`'s composite section) had its `DEFERRED_DEPTH` bind and `emissiveRectDelta` uniform upload removed as dead code; the `TAM_CLAMP` address-mode fix on `emissiveRect` was kept since it's cheap, harmless, and still technically correct insurance for plain bilinear too.

**Net result:** the depth-aware upsampling quality experiment (added, debugged, and ultimately reverted across this and the two preceding doc entries) is now fully backed out. The composite pass is back to its original, artifact-free plain-bilinear behavior. This is now considered a closed, non-viable direction for this codebase's volumetric pass - do not re-attempt bilateral/depth-aware upsampling here without first solving how to make the comparison alpha-aware (e.g. an alpha-coverage or hair-specific depth/mask input the volumetric pass could consult), which is a larger undertaking than this feature's scope.

### Volumetric/transparency ordering correction

Testing commit `37355c1f7fbba61bfbb0ffc69d3deaaba96b04a0` proved the bright
hair/window artifact predated the bilateral experiment. Fixed-camera
enabled/disabled screenshots showed volumetric RGB crossing fine alpha-blended
hair only with the effect enabled. The original call in `llviewerdisplay.cpp`
ran after `renderDeferredLighting()` returned, but that function had already
rendered non-deferred alpha geometry and dispatched Standard alpha, Exact OIT,
or AVBOIT. The additive composite therefore treated all volumetric light as if
it were in front of every transparent fragment.

The pass now runs inside `LLPipeline::renderDeferredLighting()`, after opaque
deferred/local lighting and immediately before non-deferred transparency.
Transparency consequently composites over and attenuates the volumetric
result. Because `ASVolumetricLighting::renderPass()` flushes `mRT->screen`, the
pipeline explicitly rebinds `screen_target` before continuing. The obsolete
post-`renderDeferredLighting()` call and include were removed from
`llviewerdisplay.cpp`. Plain-bilinear upsampling and shader revision `v13` are
unchanged.

This remains an approximation: all volumetric scatter is treated as behind
transparent surfaces rather than split in front of and behind each fragment.
That is preferable to the previous opposite approximation, which painted all
scatter over hair and glass. Rebuild and repeat the supplied fixed-camera hair
comparison in Standard, Exact OIT, and AVBOIT modes.

The first build of this ordering change reached the world but left the login
loading overlay visible. The log contained no shader or GL failure. Static
inspection found a definite render-target stack error: `mRT->screen` was
already bound at the insertion point, while `renderPass()` binds it for its
composite, and the pipeline then bound it again. `LLRenderTarget::bindTarget()`
explicitly forbids a target already present in its stack; the self-nesting left
later UI rendering on the wrong framebuffer. The call site now uses the
balanced sequence `screen_target->flush()`, volumetric `renderPass()`, then
`screen_target->bindTarget()`. The existing final flush after transparency
pops that single restored binding normally.

The next outdoor test fixed hair but showed a horizontal band of small black
alpha sprites/fragments. Moving the pass exposed a concrete texture-state leak:
the composite bound `sVolumetricTarget` as `DEFERRED_EMISSIVE` but never unbound
it, which was harmless only while volumetrics was the final scene draw. The
composite now explicitly calls `unbindTexture(DEFERRED_EMISSIVE)` before shader
unbind, matching the cleanup already used by Exact OIT and AVBOIT resolve
passes. This is C++ state hygiene only; shader revision remains `v13`. Retest
the same outdoor view in the active transparency mode before investigating
texture content or blend semantics further.

Additional high-intensity outdoor evidence corrected that provisional
diagnosis. The screenshot shows water consistently retaining its dark green
surface color while the volumetric layer brightens the world behind it, plus
distant alpha foliage appearing as black cutouts and becoming normal when the
camera zoom changes its alpha/LOD representation. These are coherent
transparency exclusions, not random texture corruption. The explicit texture
unbind remains required state hygiene, but cannot fix this visual result.

Moving the entire volumetric composite before transparency solved hair because
hair now attenuates the volumetric background. The same approximation also
forces water and nearly opaque distant foliage to attenuate **all** volumetric
scatter, including fog that should physically lie between those surfaces and
the camera. The former post-OIT placement made the opposite error by placing
all scatter in front. Neither single ordering can be correct without a
transparent-fragment depth/transmittance-aware split.

A pragmatic next experiment should reuse one raymarch target but split its
composite energy between pre- and post-transparency stages (for example, most
before transparency and a small configurable fraction after it). Opaque pixels
still receive the same total energy; transparent water/foliage retain some
foreground fog; hair no longer receives the full post-pass glow. This remains
heuristic but is substantially cheaper and less invasive than adding
volumetric integration to every Standard alpha, Exact OIT, and AVBOIT shader.
Do not implement or tune the split from this extreme-intensity screenshot
alone; first choose a conservative post fraction and compare normal-intensity
hair, water, and foliage in all three transparency modes.

A closer screenshot then showed a large tree and smoke particles remaining
dark at every distance, confirming that the all-before ordering—not only
foliage LOD—caused the exclusions. An 80/20 pre/post composite split was
proposed and briefly implemented but explicitly rejected as an unacceptable
artistic fudge; it and its development setting were removed before testing.

The exact reason for the black appearance is now clear. The pre-transparency
volumetric target contains the complete camera-to-opaque-depth scatter
integral. Standard alpha/OIT then blends dark foliage, smoke, or water over
that bright background, correctly attenuating the part behind the transparent
fragment, but there is no separate contribution for fog between the camera and
the transparent fragment. At extreme intensity this missing foreground
integral makes transparent surfaces read as hard dark cutouts.

A principled solution needs depth-resolved volumetric information at
transparent-fragment depth. The least invasive serious design is to retain the
pre-transparency full integral, capture a nearest-transparent-depth buffer for
Standard alpha, Exact OIT, and AVBOIT, then perform a second half-resolution
raymarch after transparency limited to that nearest depth and add only the
camera-to-transparent-surface foreground integral. Multiple transparent layers
would still be approximate, but the dominant front layer, hair, water,
foliage, and particles would receive geometrically derived foreground fog
rather than a fixed percentage. Implementing this requires a common auxiliary
depth output across all three transparency paths and must be designed with OIT
owners rather than patched into blend constants.

### Visual-priority alternative: avatar-alpha protection mask

If the desired result is specifically the original attractive post-transparency
fog over water, smoke, and foliage while preventing hair glow, a narrower and
cheaper alternative exists. Restore the volumetric composite after
transparency, but populate a screen-space coverage mask while rendering
rigged/avatar alpha. The final volumetric composite multiplies scatter by
`1 - avatar_alpha_coverage`, so hair and other avatar alpha attenuate the
otherwise post-composited fog while environmental transparency keeps the old
look.

The viewer cannot reliably classify a material as “hair”; the practical scope
is all rigged/avatar alpha, including some clothing and accessories. The mask
must be populated consistently by Standard alpha, Exact OIT, and AVBOIT paths
(and by Standard alpha on macOS), preserve fractional coverage rather than a
binary silhouette, and be cleared every frame. This is not physically complete
and deliberately prioritizes the requested visual result, but unlike a global
80/20 energy split it is spatially tied to the pixels exhibiting the regression
and leaves water, smoke, and foliage unchanged from the original appearance.

### Implemented visual-priority solution: OIT rigged-alpha depth endpoint

The user selected the original post-transparency appearance as the required
baseline: water, foliage, smoke, and glass must retain the attractive
volumetric overlay, while avatar hair must not reveal the full scatter integral
to the opaque scenery behind it.

Source inspection found a narrower input already available without adding a
new coverage render target. Standard alpha deliberately renders rigged alpha
first with depth writes enabled. Exact OIT and AVBOIT, however, deliberately
disable ordinary depth writes while capturing their fragments, so the deferred
depth sampled by the volumetric raymarch still points through hair/clothing to
the opaque wall, window, or sky behind it.

The implementation now restores the original post-transparency volumetric
placement inside `LLPipeline::renderDeferredLighting()`. After the active OIT
renderer resolves, `ASVolumetricLighting::renderRiggedDepthForPostEffects()`
re-submits only the post-water rigged-alpha draw map with color and blending
disabled and depth writes enabled. It reuses the ordinary alpha/material
shaders prepared by the pool traversal, preserving their texture alpha tests.
The volumetric pass then reads this shared depth and stops its camera ray at
avatar hair/clothing instead of integrating through to the opaque background.
Standard mode needs no extra traversal because its existing rigged pass already
writes that depth. Environmental transparency is intentionally absent from
this guide and therefore keeps the previous post-composite look.

While adding the guide, the original `depth_only` guard around emissive redraws
was restored. The OIT routing refactor had left the dispatcher call unconditional
when capture was inactive, causing DoF or post-effect depth traversals to submit
glow batches despite color writes being masked. Depth-only traversal now skips
those redraws as intended.

The depth-guide implementation and GL state live in the AS-owned
`asvolumetriclighting` module. The upstream/shared alpha pool exposes no new
feature-specific method; its only functional edit is the restored one-line
`depth_only` emissive guard. The pipeline retains only the scheduling call.

This is a visual-priority, avatar-scoped solution rather than a physically
complete multi-layer volumetric/transparency integrator. Its expected result is
geometrically better than a binary coverage mask: the camera-to-hair foreground
portion of the ray remains visible, while the much larger hair-to-background
portion is excluded. It applies to all rigged alpha (including translucent
clothing/accessories), because the renderer has no reliable semantic “hair”
classification. No shader-cache revision was bumped.

**Required runtime verification:** clear the shader cache, build, and use the
same fixed camera/high-intensity scene. In Exact OIT first, confirm (1) hair no
longer shows the window streak, (2) water, smoke, and distant foliage match the
old attractive post-pass behavior, and (3) no loading-overlay/render-target
regression. Then repeat hair in AVBOIT and Standard. Pay particular attention
to partially transparent rigged clothing: it will currently create the same
alpha-tested depth endpoint as hair, matching Standard's longstanding rigged
depth behavior rather than preserving fractional transmittance.

**Runtime result, corrected after direct enabled/disabled comparison:** outdoor
transparency is visually correct again. Avatar hair is not itself being
darkened: the pixels inside the marked hair region are effectively identical
to rendering with volumetrics disabled. It *appears* much darker because the
surrounding sky and scene receive strong scatter while the rigged-depth guide
limits hair pixels to only the short camera-to-hair integral. Against a bright
window the contrast is smaller, explaining why that case looks slightly better.

The real missing term is volumetric scatter behind the fractionally transparent
hair, attenuated by the hair's resolved transmittance. If `Vfull` is the full
camera-to-opaque-depth integral, `Vfront` is the camera-to-hair-depth integral,
and `T` is the OIT/alpha transmittance through hair, the desired contribution is
approximately `Vfront + T * (Vfull - Vfront)`. The current depth-guide result is
only `Vfront`; the original post-pass result was only `Vfull`. This is contrast
from a missing transmitted contribution, not destructive RGB darkening.

The cancelled depth-aware/bilateral half-resolution upsample cannot solve this.
It only chooses scatter samples using opaque deferred-depth similarity during
the final half-resolution upsample. It neither captures avatar-alpha coverage
nor restores fractional OIT transmittance; previous testing also showed that
its opaque-depth assumptions introduced the original hair silhouette streak.
Do not restore it for this regression.

The depth endpoint remains useful as `Vfront`, but is insufficient alone.
Keeping the correct outdoor post-transparency placement requires a fractional
avatar coverage/transmittance input so the composite can interpolate between
`Vfront` and `Vfull`. The next viable direction is the AS-owned coverage target
described above, populated consistently by Standard, Exact OIT, and AVBOIT (or
derived from each OIT renderer's resolved transmittance where available).

### Depth-resolved transparency implementation (in progress)

The final design does not use an avatar mask or special-case hair. The normal
far-to-near alpha equation supplies the correct multi-layer result if the full
camera-to-opaque integral is already behind transparency and every transparent
source color includes the cumulative camera-to-that-fragment integral. For two
or more layers, ordinary recursive alpha blending expands to the correct
piecewise volumetric segments automatically.

To avoid raymarching separately for every transparent fragment, the AS module
now builds a screen-sized 4x4 atlas containing 16 cumulative depth slices. Each
tile is quarter resolution; total storage is therefore one full-resolution
RGBA16F texture. Slice distances use a quadratic distribution over the existing
128 m march range, concentrating precision near avatars and nearby geometry.
Transparent shaders derive their view-space distance, interpolate the adjacent
atlas tiles, and add that foreground scatter to their straight source RGB.

`alphaF.glsl`, `pbralphaF.glsl`, and the alpha permutation of
`fullbrightF.glsl` contain the shared sampling operation. These same sources are
compiled for Standard, Exact OIT, and AVBOIT, so both OIT implementations retain
their existing capture/resolve algorithms and node formats. Atlas allocation,
generation, and texture binding live in `asvolumetriclighting`; the shared alpha
pool has only one AS binding call in its existing shader-preparation function.
The temporary rigged-depth endpoint was removed. The full volumetric composite
again runs immediately before transparency with a balanced screen-target
flush/bind sequence.

This implementation is **not ready for a user build yet**. Directional
sun/moon scatter is wired, but the atlas must still incorporate optional local
lights, and transparent water/any alpha shader outside the three common alpha
families must be audited and hooked before claiming consistent rendering.
Static shader/API review and `git diff --check` are also required after those
paths are complete. Shader revision remains unchanged during development.

**First atlas runtime result:** the directional debug modes remained
scene-shaped and mode 7 remained finite/green, so the established depth and
inverse-projection path did not regress. Mode 0 instead painted a repeated
orange material texture across alpha hair and produced green fragments on
distant transparent geometry. This is texture-unit aliasing, not scatter: the
atlas was bound during `prepare_alpha_shader()`, after which preparation of
other shaders and normal per-material texture setup reused the unit. At draw
time `asVolumetricAtlas` therefore sampled whichever diffuse/material texture
was left on that channel.

The alpha traversal now rebinds the AS atlas immediately after each actual
PBR or ordinary alpha shader switch, before that draw's material submission.
The preparation-time bind remains responsible for initializing the enabled
uniform, while the draw-time bind guarantees the sampler sees the atlas rather
than stale material state. All additions to `lldrawpoolalpha.cpp` and shared
shader sources are enclosed in ownership tags; shader revision remains `v13`.

**Correction after retest:** draw-time rebinding made no visual difference.
Inspection of `LLGLSLShader` found the deeper API mismatch. Custom uniforms are
recorded in `mUniformMap`, but custom samplers are not assigned entries in the
reserved `mTexture` table. The string render-target `bindTexture()` overload
looks up a raw OpenGL uniform location and forwards that integer to
`bindTexture(S32)`, which interprets it as a reserved-uniform index. It therefore
binds an unrelated mapped texture channel; repeating that call cannot repair
the alias.

`bindTransparencyAtlas()` now handles this custom sampler explicitly. It uses
`shader.mActiveTextureChannels` (the first channel after every sampler mapped by
the shader), assigns that channel to `asVolumetricAtlas` via `glUniform1i`, and
binds the AS render-target texture manually with bilinear filtering and clamp
addressing. This avoids modifying the global reserved-uniform enumeration and
keeps the implementation in the AS module. Retest mode 0; the previous
draw-time call sites remain useful because they restore this explicit binding
after later shader preparation changes texture state.

**Explicit binding retest:** the orange/green foreign-texture pattern is gone,
confirming that the atlas sampler now reads the intended AS target. Hair and
some distant alpha objects instead receive an excessive pale blue-white
foreground contribution. This is real atlas content, not another material map.
Do not tune intensity yet: the atlas slice values/mapping must be inspected.

Mode 2/3 showed broad horizontal bands. Atlas sampling is disabled whenever
debug mode is nonzero, so these cannot be the 16 atlas depth slices. An initial
diagnosis attributed them entirely to transparent avatar layers composited over
the extreme replacement image. The user correctly observed that the bands also
continue through the surrounding scene, disproving that explanation as the sole
cause. They are directional raymarch/shadow-sampling structure—likely sparse
fixed-step sampling amplified by raw occlusion/visibility display, with shadow
cascade transitions also a possible contributor. Transparency can make them
more conspicuous on the avatar, but does not create the scene-wide bands.

Therefore the bands are a real diagnostic signal/quality issue, although raw
binary-like modes 2 and 3 exaggerate them compared with mode 0's colored,
half-resolution, bilinearly upsampled result. They are independent of the new
transparency atlas and should be addressed separately after mode-0 transparency
composition is stable (e.g. temporally varying/stable blue-noise sampling or a
better-integrated sample distribution, with cascade-boundary checks).

The user counted approximately 16 horizontal bands regardless of camera zoom.
This strongly identifies sample quantization rather than the four shadow
cascades: the active directional march uses 16 samples, and modes 2/3 display
the arithmetic mean of mostly binary shadow comparisons. Such a mean has only
17 possible levels (`0/16` through `16/16`), naturally forming roughly 16
contours. Per-pixel jitter moves where samples land but does not increase the
number of possible mean values. This is expected for the current diagnostic
estimator, but also documents a real low-sample quality ceiling. Mode 0 hides it
with colored scaling, spatial jitter, half-resolution rendering, and bilinear
upsampling; eliminating it rather than hiding it requires more samples,
temporal accumulation, or filtering the visibility estimate—not cascade tuning.

The atlas debug mode was briefly removed after concern about proliferating
diagnostics, then restored as mode 10 after the user clarified that modes are
welcome when genuinely useful. Mode 10 directly displays the 4x4 cumulative
atlas (near-to-far, left-to-right then top-to-bottom), uniquely separating bad
slice generation from bad sampling in transparent shaders. Do not add further
modes without an equally specific diagnostic question. No shader revision bump
during development.

The first XUI edit accidentally matched the earlier Exact OIT spinner: it raised
Exact OIT's maximum to 10 while leaving the volumetric spinner at 9, even though
the volumetric tooltip documented mode 10. Corrected with control-specific
context: Exact OIT is again capped at 9 and volumetric lighting is capped at 10.

The first mode-10 capture appeared almost uniformly pale blue, with ordinary
transparent scene fragments subsequently drawn over it. HDR exposure flattened
the small raw floating-point atlas values, so this display could not reveal
slice magnitude or even make tile boundaries reliable. Mode 10 now asks the
atlas shader for exposure-resistant false color: red stores raw scatter, green
stores 16x scatter, blue stores 256x scatter, all clamped; thin red tile borders
make the 4x4 layout explicit. Near slices should start predominantly blue and
progress smoothly toward cyan/white. This changes diagnostic output only;
mode-0 atlas RGB remains the physical light-colored scatter.

The false-color capture exposed a definite generation error: atlas brightness
was not monotonic; later/farther tiles, notably the final row, became darker.
The former implementation gave every slice the same small sample count spread
over that slice's entire camera interval. Those independent estimates did not
share samples, so a far slice could miss a lit near region counted by an earlier
slice and report a smaller value—mathematically impossible for cumulative
nonnegative scattering and the direct cause of inconsistent hair/distant-alpha
foreground values.

Atlas generation now integrates the 16 quadratic depth segments explicitly.
Slice N recomputes the identical segments `0..N` with a stable per-screen-ray,
per-segment jitter; slice N+1 contains those same nonnegative terms plus exactly
one new segment. Scatter is derived from the accumulated visibility-distance
integral divided by the common 128 m normalization. This guarantees monotonic
cumulative values by construction, removes the obsolete atlas `sample_count`,
and costs an average 8.5 shadow samples per atlas pixel (the whole 4x4 atlas is
one screen's worth of pixels). Mode 0 and mode 10 require retesting; no shader
revision bump during development.

The next mode-10 capture supports the cumulative fix. The display order had
been documented backwards because OpenGL texture origin is bottom-left: slices
0..3 are the bottom row, then progression moves upward. The XUI tooltip now
states the correct order. A later viewpoint corrected an imprecise description
that entire tiles should become “progressively brighter cyan.” The actual
invariant is per corresponding local pixel/ray: its encoded cumulative scatter
must not decrease in later slices. Shadowed rays may remain black across several
slices, while newly encountered lit segments add spatially structured blue/cyan
regions. Whole-tile average color and uniform cyan brightness are not required.
Normal transparent geometry is also drawn after the diagnostic and must not be
mistaken for atlas contents.

Mode 0 is substantially improved: avatar hair no longer receives the extreme
white-blue atlas value. Remaining black and colored distant fragments are
shader-coverage gaps. In particular, blended legacy materials use class-3
`materialF.glsl`, which precomposited full background scatter but lacked the
camera-to-fragment atlas term already present in alpha/PBR/fullbright alpha.
Added the same tagged cumulative sampling hook to the blend-only branch.
Standard and Exact OIT add it directly to source RGB; AVBOIT adds it after its
legacy specular-glare rescaling so glare cannot incorrectly amplify foreground
fog. Specialized transparency and local-light atlas coverage still require the
planned audit.

**Fullbright coverage finding:** after the legacy-material hook, the remaining
runtime defects are concentrated on distant saturated windows, signs, flags,
and small colored objects. The post-deferred ordering confirms these are
consistent with fullbright geometry rather than another atlas-generation
failure. Both `POOL_FULLBRIGHT` and `POOL_FULLBRIGHT_ALPHA_MASK` render after
the full volumetric composite, but `fullbrightF.glsl` previously added the
camera-to-fragment integral only for its `IS_ALPHA` permutation, and the two
fullbright pools did not bind the atlas at all. Consequently ordinary and
alpha-masked fullbright surfaces overwrote the fogged background with their
unmodified saturated color.

The cumulative foreground term now applies to every non-HUD fullbright
permutation, and the ordinary/alpha-mask pools bind the atlas after selecting
each static or rigged shader variant. HUD rendering remains excluded. The glow
pass is deliberately untouched: it is an additive bloom contribution layered
on an already-rendered surface, so adding foreground scatter there would count
the same fog a second time. Retest the previously boxed flag, windows, and signs
in mode 0; no shader-cache revision bump was made during development.

### Opaque silhouette aliasing after the depth-resolved redesign

A close avatar capture shows visible one-pixel staircase teeth along both outer
shoulders only while volumetric lighting is enabled. The underlying avatar is
smooth without the effect. This identifies the half-resolution volumetric
composite as reintroducing a low-resolution opaque/background boundary; it is
not aliasing in the avatar mesh or material.

The earlier bilateral experiment was invalid in the old architecture because
it ran against already-rendered transparent hair while deferred depth could
only describe the opaque surface behind that hair. The current architecture
changes that premise: the full volumetric field is composited against opaque
depth first, and transparent hair is rendered afterward with its independently
depth-resolved atlas contribution. Deferred depth is therefore an appropriate
guide at the opaque composite stage now.

Mode 0 again uses a four-tap depth-aware upsample, while every diagnostic mode
retains its unmodified plain texture sample. This implementation avoids the old
unbounded reciprocal weighting and nonlinear raw-device-depth epsilon. It
reconstructs view-space Z for the full-resolution destination and four
half-resolution source centers, weights the normal bilinear footprint by a
bounded exponential of relative view-depth difference, and normalizes the
result. Relative depth makes the rejection behave consistently for nearby and
distant silhouettes. The composite program now binds deferred depth and
receives the actual selected source texture size. If a subpixel opaque feature
has no depth-compatible half-resolution tap, the shader explicitly falls back
to the former bilinear sample rather than normalizing negligible weights into a
black pinhole. Retest shoulders, hair against sky/windows, foliage, and thin
distant geometry; no shader revision bump was made.

### Specialized water composition gap

The depth-aware mode-0 retest confirms smooth avatar shoulders. Two outdoor
captures still show region water as a sharply bounded, nearly black/green layer
against the fogged landscape, most strongly at grazing/reflection-heavy viewing
angles. This is not an upsampling failure. `LLDrawPoolWater` runs after the full
volumetric composite with blending disabled. Transparent water first copies the
current framebuffer (which already includes full volumetric scatter), then
`waterF.glsl` mixes that refracted copy with reflection/radiance according to
Fresnel and shoreline fade. The reflection portion replaces the copied
foreground scatter and previously never restored the camera-to-water term.

Adding the complete foreground atlas value to all water would be wrong because
the framebuffer/refraction portion already contains volumetrics. From the
shader's two nested mixes, the retained framebuffer coefficient is
`1 - fade * fresnel`; therefore the missing fraction is exactly
`fade * fresnel`. The class-3 water shader now adds camera-to-water cumulative
scatter multiplied by that missing fraction for transparent water. Opaque
fallback water, whose synthetic background contains no precomposited scatter,
receives the full foreground term. The water pool binds the atlas immediately
before its draw. Both upstream edits are ownership-tagged, and disabled
volumetrics still set the helper to return exact zero. Retest grazing and
near-normal views, shorelines, reflections, and underwater rendering; no shader
revision bump was made.

### Debug mode 1 late-geometry silhouettes

A pre-build mode-1 capture shows dark bushes/foliage and smoke particles over
the raw scatter visualization. This is expected from the diagnostic's current
placement and is not, by itself, a mode-0 shader-coverage failure. Mode 1
replaces the opaque screen with the raw half-resolution scatter target at the
normal pre-transparency volumetric call site. The regular post-deferred pass
then draws water, foliage, particles, and other transparency over that replaced
background. `bindTransparencyAtlas()` deliberately disables foreground-atlas
addition for every nonzero debug mode, preventing diagnostic data from being
fed back into ordinary material color. Consequently late geometry remains
visible in its normal source colors and often appears nearly black against the
raw mode-1 field.

Do not use these silhouettes alone to justify another production shader hook.
Judge foliage and smoke coverage in mode 0. A future cleanup could move the
debug replacement display to the end of scene rendering (or suppress late
geometry only while showing diagnostics) to provide an uncontaminated
full-screen readout, but that is a debug-presentation improvement rather than a
mode-0 correctness fix and is not included in the current build round.

### Pre-build water wave-pattern observation

Another mode-0 capture from the still-running old build shows conspicuous
repeating vertical/wave bands across the dark water. The user explicitly took
this before rebuilding, so it does **not** contain the pending specialized-water
fix and cannot be used to evaluate that fix. The pattern follows the existing
water normal/refraction direction rather than obvious 4x4 atlas tile boundaries.
Do not add a second speculative compensation based on this capture. First test
the pending Fresnel-weighted foreground restoration at the same viewpoint; then
compare whether the dark cutout and pattern improve, remain, or worsen.

**Correction after user clarified the build state:** the dark-water and
repeating-pattern captures do include the pending water hook. The current
session log is fresh and reports `Loaded water shaders`, with no compile/link
failure, so the hook is active. Static inspection found another definite HDR
integration error at the final water output: stock `waterF.glsl` clamps the
entire `frag_color`, including RGB, to `1.0`. The surrounding screen and AS
scatter are HDR and are tonemapped later. Water therefore discarded precisely
the high-range scatter needed to match the fogged surroundings; wave/Fresnel
variation reaching that ceiling unevenly can also emphasize repetition.

When `asVolumetricEnabled != 0`, water now preserves nonnegative HDR RGB and
clamps only output alpha to `[0,1]`. When volumetrics are disabled, the original
whole-`vec4` clamp executes unchanged, preserving the existing vanilla/Exact
OIT/AVBOIT-off appearance. Retest the same wide overhead and grazing views. The
earlier note that the screenshots predated the water hook was based on a
misunderstanding and is superseded by this correction.

**HDR-clamp retest:** preserving water HDR produced little visible change; the
surface remained a dark cutout. The clamp was a real incompatibility but not the
dominant composition error. The Fresnel-weighted restoration is therefore
replaced by an exact directional decomposition using the existing cumulative
atlas. Transparent water's `screenTex` already contains the full volumetric
integral to the refracted opaque point. Immediately after sampling it, the
shader now subtracts `V(distort2, refPos distance)`. All ordinary water
refraction/reflection/shoreline shading then operates on the scene contribution
without precomposited directional fog. After those mixes, the shader adds
`V(current pixel, water-surface distance)` exactly once.

This directly represents camera-to-water foreground scatter and retains no
Fresnel heuristic. It should also remove the repeating water-normal imprint
caused by mixing spatially displaced precomposited fog. The atlas helper now
accepts explicit screen UV and distance so both endpoints use the same
cumulative representation. Optional local-light scatter is not yet present in
the transparency atlas, so this exact subtraction/recomposition currently
applies to directional sun/moon scatter; local-light water consistency remains
part of the documented atlas-coverage audit. The HDR-output preservation stays
in place because water must not clamp newly added scatter before scene
tonemapping. A static follow-up also recomputes `refPos` after stock water's
possible `distort2 = distort` fallback; otherwise subtraction could pair the
fallback UV with the rejected distorted endpoint. No shader revision bump was
made.

### Missing volumetric extinction/transmittance

A high-altitude mode-0 capture shows distant smoke and saturated fullbright
objects remaining conspicuously visible through a very dense volumetric field.
This is not another late-shader atlas binding omission. Static inspection
confirms the current model computes and composites additive in-scattering only:
the directional raymarch/atlas store scatter RGB, local lights add scatter RGB,
and transparent/fullbright shaders add camera-to-fragment scatter to source
color. No pass computes Beer-Lambert transmittance and no surface/emissive RGB
is attenuated by the medium. Bright emission can therefore remain visible no
matter how dense the added haze becomes.

Do not patch this with a fullbright-only distance fade. The correct extension
is systemic: carry cumulative transmittance alongside scatter (the existing
RGBA16F targets provide an alpha channel), composite opaque color as
`C * Tfull + Vfull`, and emit every late fragment as
`C * Tfront + Vfront`. Ordinary alpha recursion then remains correct for
Standard, Exact OIT, and AVBOIT: blending that source over the fully
precomposited destination yields the required foreground medium plus attenuated
surface/background segments. Water must use the same resolved full-field
handoff, and local-light policy must be explicit (lights illuminate the shared
medium but do not independently create extinction).

This needs a defined extinction coefficient/control and coordinated changes to
the opaque composite plus every atlas consumer. It is deliberately not folded
into the pending water build as a narrow smoke/fullbright heuristic. The next
implementation phase should add and validate transmittance end-to-end, including
disabled-feature identity, before claiming distant emissive correctness.

### Exact resolved-field handoff for near water

The atlas-subtraction build is substantially better from high altitude, but a
near-water capture shows large vertically warped blocks across the surface.
This is a deterministic estimator mismatch: water subtracted the quarter-
resolution cumulative atlas value from a framebuffer containing the separately
raymarched half-resolution field after depth-aware full-resolution upsampling.
Both approximate the same directional integral, but their sample patterns and
resolutions are not identical. Refraction distortion makes the residual
especially visible nearby; distance/filtering hides it in the aerial view.

The AS module now owns an additional full-resolution RGBA16F resolved target.
In normal mode, the half-resolution directional-plus-local field is first
depth-aware-upsampled into this target with replacement blending. That exact
resolved texture is then additively drawn into the scene without another
filtering transform. The water shader receives the same resolved target and
subtracts `texture(asVolumetricFull, distort2)` from its copied refracted
framebuffer before stock water shading. It then adds the cumulative
camera-to-water atlas value. Thus subtraction is sample-identical to what was
added, eliminating the near-field residual rather than attempting to tune it.

Debug modes retain their former direct replacement display and do not use the
resolved handoff. Disabled volumetrics still skip the AS pass and set the water
helper to zero, leaving original water output active. This costs one additional
full-resolution RGBA16F target and one fullscreen copy in mode 0, chosen for
correctness and stable water/refraction behavior. No shader revision bump was
made.

**Resolved-field retest:** both aerial and near captures remained approximately
unchanged, including the vertically repeated near-water pattern. The RTX 3080
Ti exposes 32 fragment texture units, but the water shader is sampler-heavy
(reflection probes, shadows, refraction/depth, exclusion, and two normal maps).
The AS binder incorrectly assumed `mActiveTextureChannels` and the following
unit were free for its two custom samplers. If either appended unit is outside
the shader's usable mapped set, the custom sampler retains its link-time value
while the intended texture is never bound; sampling an aliased water/normal
texture explains both the lack of correction and the repeated wave pattern.

Shader creation already assigns every active custom sampler a unique texture
channel even though custom names are not represented in the reserved
`mTexture[]` lookup table. `bindTransparencyAtlas()` now obtains each custom
sampler's assigned channel with `glGetUniformiv(program, location, &channel)`
and binds the atlas/resolved target to that exact unit. This avoids both prior
API errors: a raw uniform location is not passed as a reserved index, and no
new unit is appended after a sampler-heavy shader's active set. Channel values
are range-checked before binding. This correction applies to all atlas
consumers and should finally make water's resolved-field subtraction sample the
AS texture rather than an aliased material/water texture. No shader revision
bump was made.

**Sampler-binding regression and scope correction:** water improved after using
its link-assigned sampler channels, but the same global change catastrophically
corrupted generic alpha/fullbright rendering: avatar hair and distant foliage,
lights, and alpha objects became white or sampled foreign material content.
Those shaders use indexed/dynamic material texture submission after shader
binding, which reuses link-mapped texture units. A custom atlas bound to its
link-assigned unit is consequently overwritten before the draw. The previously
working appended channel at `mActiveTextureChannels` is required for these
dynamic material families.

The queried-channel path is now restricted to shaders that declare
`asVolumetricFull`, currently the fixed-sampler water shader. Water queries and
binds both `asVolumetricAtlas` and `asVolumetricFull` on their link-assigned
units. Every generic alpha, PBR alpha, legacy material, and fullbright consumer
again assigns the atlas to the proven appended channel and rebinds it at its
existing draw-time call sites. This preserves the water improvement without
changing the dynamic-material binding contract. Retest hair first, then alpha
foliage/fullbright and water. No shader revision bump was made.

### Water-only filtering of moving foliage shadow stencils

After scoping sampler binding, avatar hair/material alpha returned to normal and
water improved, confirming both binding paths are now active. A remaining near
capture shows large tree-leaf silhouettes moving over the smooth water surface
with camera motion. These are no longer an aliased foreign texture or resolved-
field subtraction residual. They are real directional-shadow visibility in the
quarter-resolution cumulative foreground atlas. Scene detail hides this sparse
shadow structure on most alpha surfaces, while smooth dark water exposes it as
a high-contrast screen-space stencil.

Water now filters each atlas depth slice with a five-tap cross kernel (center
weight 4 plus four taps eight full-resolution pixels away, normalized by 8)
before interpolating between cumulative depth slices. Tap UVs are clamped inside
the current tile's valid screen-UV range, so filtering cannot bleed from one
depth slice into another. This preserves broad volumetric shadowing while
softening foliage-scale sample structure and camera movement. The filter exists
only in `waterF.glsl`; generic alpha, PBR, material, and fullbright atlas reads
remain unchanged. No shader revision bump was made.

**Superseded before build by water-level evidence:** a closer capture reveals
broad horizontal/curved bands and hard depth transitions over the water, in
addition to foliage-shaped structure. The smooth near-horizontal plane crosses
the atlas's 16 quadratic cumulative depth slices over large screen areas.
Spatially filtering within a slice cannot remove that depth quantization, so
the pending five-tap water filter was removed before testing.

Water no longer samples the 16-slice foreground atlas. Its class-3 shader now
computes the camera-to-water directional integral continuously per fragment
with 16 dithered shadow samples, using the same Henyey-Greenstein phase,
128-metre cap, sun/moon selection, visibility integration, intensity, and
asymmetry as the AS directional implementation. `bindTransparencyAtlas()`
uploads the two artist controls to water and binds only `asVolumetricFull` on
water's fixed link-assigned sampler channel. The exact resolved background
subtraction remains unchanged. Generic transparency retains the atlas and its
proven appended-channel binding.

This deliberately spends more GPU time on visible water in exchange for
continuous depth and full screen-space resolution: no cumulative slice
boundary can cross the water plane, and shadow structure is dithered per water
fragment rather than enlarged from a quarter-resolution atlas tile. Other
materials are unaffected. No shader revision bump was made.

### Main directional-ray quality tier

A current-build water-level capture also shows chunky, low-resolution
foliage-shaped rays in the surrounding air, not only on water. This is the main
directional field: it remained fixed at half resolution and used 24 samples at
the highest shadow setting. The pending continuous water foreground march only
addresses water-surface depth slicing and cannot improve this shared field.

Volumetric resolution/sample count now follows the existing shadow-quality
choice. `RenderShadowDetail >= 2` allocates the main target at full screen
resolution and uses 32 directional samples; lower shadow detail retains the
half-resolution 16-sample path. When source and resolved dimensions already
match, the resolved-field pass disables its unnecessary four-tap depth-aware
upsample and performs a plain exact copy. This substantially increases GPU cost
at highest shadow detail (roughly four times the raymarch pixels plus the higher
sample count) but keeps the lower tier and disabled-feature behavior unchanged.
No shader revision bump was made.

**Quality-control correction before build:** the user correctly objected that
silently coupling an approximately fivefold raymarch cost increase to shadow
detail is inappropriate. Added persisted Boolean
`RenderVolumetricLightingHighQuality`, exposed beside the other volumetric
controls as **High quality rays (full resolution)**. It defaults off:
half-resolution, 16 directional samples. Enabling it selects full resolution
and 32 samples, with a tooltip warning that GPU cost rises significantly.

The setting is independent of OIT and remains visible on macOS with the rest of
volumetric lighting. `renderPass()` detects a changed tier and reallocates only
the AS-owned main volumetric source target after the caller has flushed the
screen, so the choice applies live without rebuilding all viewer graphics
buffers. The resolved full-resolution handoff and transparency atlas keep their
existing sizes. The earlier shadow-detail-coupled policy is superseded. No
shader revision bump was made.

### Correction: apparent ground-origin shafts are sun rays

The initial diagnosis of a rebuilt dusk capture as local-light fog was wrong;
the user correctly identified the shafts as sun rays. The local-light shader is
unshadowed and integrates smooth bounded light spheres. It can leak through
terrain, but cannot generate the strongly occlusion-defined crepuscular shafts
visible in this capture. Do not use radial-looking screen-space convergence
alone to classify a volumetric source.

The capture is consistent with a low-elevation sun hidden behind the terrain or
rock edge: parallel three-dimensional rays project as a fan around the nearby
occluder. The persisted sun/moon intensity is also extremely high at about
`6.68`; the resulting HDR in-scatter can reduce the sun disk's contrast until
it disappears into the bright surrounding sky. The high-quality tier changes
only target resolution (half to full) and directional sample count (16 to 32),
not sun/moon direction, selection, or sky-disk rendering. It can nevertheless
make terrain/foliage shadow structure far sharper and expose an apparent source
location that the former half-resolution field blurred.

The user's more precise observation supersedes the missing/displaced-sun
interpretation: the real sun remains correctly placed and produces normal
shafts when viewed toward it. A second set appears to converge at the point
opposite the sun. This is the anti-solar vanishing point, not a second light or
a sign error by itself. Parallel world-space shafts project toward opposite
vanishing points depending on viewing direction; with a low sun, the anti-solar
point lies near the opposite horizon and can look like a source on the ground.

The question is therefore prominence, not position. The shader's phase
convention is internally consistent: `ray_dir` points camera-to-sample,
`light_dir` points sample-to-sun, and their dot product is also the dot product
of the photon travel direction with the sample-to-camera direction. Positive
Henyey-Greenstein `g` correctly favors the solar view and suppresses the
anti-solar view. Very high intensity can nevertheless expose weak backward
scatter and its sharply defined shadow structure. Do not mirror or negate the
light direction to remove physically valid anti-crepuscular geometry; assess it
at the intended intensity first, then tune phase/asymmetry if it remains too
strong.

### Unity `VolumetricLight.shader` reference review

The root-level Unity reference independently confirms the directional phase
sign. It evaluates `dot(_LightDir, -rayDir)`, where Unity's directional-light
vector follows the photon travel direction; AyaneStorm's `light_dir` points
toward the light, so negating both operands gives the existing equivalent
`dot(light_dir, ray_dir)`. Copying the Unity expression literally without
accounting for this convention would mirror the phase lobe and be incorrect.

The materially useful difference is extinction/transmittance. The reference
integrates density-scaled scattering and extinction per step, weights each
contribution by `exp(-extinction)`, and returns accumulated transmittance in
alpha. AyaneStorm presently accumulates mean shadow visibility, multiplies it
by phase/intensity/distance, then clamps; it has no optical-depth attenuation.
Consequently long weak paths—including physically valid anti-solar shafts—can
remain too visible, especially with elevated intensity. This reinforces the
existing “Missing volumetric extinction/transmittance” finding. A reliable
quality fix should implement a coherent scattering/extinction model rather
than hide the anti-solar point by reversing directions or applying a
screen-space mask.

Other reference features are optional appearance controls rather than direct
bug fixes: world-space density noise, exponential height fog, bounded point and
spotlight volumes, and texture dithering. Its Unity cascade selection, light
volumes, coordinate reconstruction, and blend setup are engine-specific and
must not be transplanted directly.

### Directional view-path extinction implementation

Implemented the transferable Beer-Lambert part of the Unity reference without
altering sun direction, phase convention, shadow coordinates, or shader cache
revision. Each valid directional shadow sample is now weighted by
`exp(-extinction * sample_distance)` before its segment length is accumulated.
The same formula and setting are used by the main directional target, the
16-slice transparency atlas, and water's direct camera-to-water foreground
march, preventing those paths from disagreeing at transparent boundaries.

Added persisted `RenderVolumetricLightingExtinction`, exposed as editable
**Distance extinction** with range `0.000..0.250`, increment `0.001`, and a
conservative default of `0.012` per view-space metre. Zero exactly restores the
former unattenuated accumulation for comparison. The setting affects only
sun/moon volumetrics; bounded local-light fog retains its existing range-based
falloff. Raw visibility/occlusion debug modes 2 and 3 intentionally remain
unattenuated so they continue to diagnose shadow sampling rather than the
appearance model. Mode 1 and atlas mode 10 show the attenuated result.

The explicit numeric-entry width is `60` pixels so all three decimals remain
visible. The expanded maximum is an artist/development range rather than a
physical limit: at `0.25`, the e-folding distance is only 4 metres and distant
directional scatter is effectively eliminated.

This implements attenuation of directional in-scatter along the camera path,
which directly addresses over-prominent long/anti-solar shafts. It does not yet
multiply the underlying scene by medium transmittance; doing that would change
the full scene/alpha/water compositing equation and should not be smuggled into
this targeted correction without separate validation. The requested shipped
sun/moon intensity default is also corrected from `0.5` to `0.8`; persisted
user values remain unchanged.

### Bilateral blur applicability

A bilateral blur smooths neighboring volumetric samples only when both their
screen-space distance and a guide signal—normally scene depth, optionally
normal—are similar. Unlike ordinary Gaussian blur, it can reduce raymarch
dither/noise and half-resolution stair-stepping without freely bleeding light
across opaque silhouettes. It is distinct from the cancelled bilateral
**upsampling** experiment: upsampling chooses how a low-resolution pixel is
reconstructed at full resolution, while a bilateral blur filters an already
sampled volumetric field (although implementations often combine the two).

It cannot correct the anti-solar convergence or excessive long-path energy;
those are geometry/transport issues addressed by phase and extinction. It may
help residual banding, grain, jagged shaft edges, and low-resolution shimmer in
the default quality tier. However, a depth-guided blur using only the deferred
opaque depth has the same structural limitation previously exposed by hair,
foliage, smoke, fullbright alpha, and water: those surfaces may be absent or
misrepresented in the guide, allowing the filter to mix foreground and
background scatter across transparent silhouettes.

Therefore do not add a generic deferred-depth bilateral blur to the final
composite. A safe later experiment would filter the AS directional source
before transparency correction, use a small separable radius, combine opaque
depth rejection with the existing transparent depth-resolved information, and
guarantee a strict bypass when volumetrics are disabled. High-quality
full-resolution rendering needs less such filtering and should be the baseline
for judging whether the added complexity is worthwhile.

#### Root-level `BilateralBlur.shader` review

The Unity reference is not merely a blur. It implements a complete resolution
pipeline: checkerboard min/max depth downsampling, horizontal and vertical
separable bilateral filtering, then a depth-aware upsample that uses bilinear
color only away from depth edges and otherwise selects the low-resolution tap
whose eye depth is closest to the full-resolution opaque depth.

Transferable idea: use two one-dimensional passes rather than a square 2-D
kernel. Its half-resolution radius 5 costs 11 color and 11 depth samples per
pixel per direction, far less than an equivalent 11x11 kernel but still a
material extra GPU pass pair. The full-resolution radius 7 costs 15 taps per
direction. The Gaussian spatial weights are combined with
`exp(-(depthDifference * 0.5)^2)`.

Non-transferable assumptions:

- The guide contains only Unity camera depth; it has no alpha-layer coverage or
  depth and therefore cannot protect Second Life hair, foliage, smoke,
  fullbright alpha, or water.
- Checkerboard min/max depth intentionally alternates which surface wins inside
  each 2x2 block. That can retain thin opaque edges in Unity's later nearest-tap
  upsample, but would introduce a patterned foreground/background choice into
  AyaneStorm's already depth-sensitive transparency handoff.
- The upsample threshold is a fixed absolute eye-depth sum (`1.5`) and is not
  suitable unchanged for the viewer's scale, far distances, or reconstructed
  transparent depths.
- Filtering only the main volumetric target would make it disagree with the
  unfiltered cumulative transparency atlas and water foreground reconstruction.
  All consumers would need a mathematically compatible filtered field.

Conclusion: the separable-kernel structure can inspire a future quality filter,
but this shader as written would revive the exact class of transparency defects
already encountered. Do not port it as the current anti-solar/extinction fix.

### Remaining transferable ideas from the Unity references

Static comparison against the current AS shaders leaves the following useful
items, in priority order:

1. ~~**End-to-end transmittance.**~~ **Done (2026-08-22).** Implemented in full:
   the transparency atlas now carries per-tile Beer-Lambert `T` in its alpha
   channel (`asVolumetricAtlasF.glsl`), all five upstream consumers (alphaF,
   pbralphaF, materialF, fullbrightF, waterF) attenuate their own scene-color
   term by `T` via a per-file `asVolumetricTransmittance()`/
   `asVolumetricWaterTransmittance()` helper before adding scatter, and the
   opaque composite (`asVolumetricCompositeF.glsl`) attenuates the full scene
   via a scene-copy target with a smooth fade-to-1.0 band approaching the
   128 m sky/ground boundary so the sky itself is never double-attenuated.
   Water attenuates only its Fresnel-reflected share, since the refracted
   share already receives scene attenuation for free via `screenTex`. See
   (deleted) `doc/ayanestorm-volumetric-transmittance-plan.md` for the
   original scoping if the design rationale is needed again.
2. ~~**Independent scattering and extinction coefficients.**~~ **Done
   (2026-08-22).** Replaced `RenderVolumetricLightingIntensity` (unrelated
   artist multiplier) and `RenderVolumetricLightingExtinction` with a
   physically-derived `RenderVolumetricLightingDensity`/
   `RenderVolumetricLightingAlbedo` pair: `density` keeps extinction's exact
   Beer-Lambert role (`exp(-density * distance)`, same range/behavior as the
   old extinction setting), and `albedo` is a new `[0,1]` single-scattering
   fraction. Final scatter brightness is now `density * albedo *
   BRIGHTNESS_SCALE * phase * (visibility_integral / MAX_MARCH_DISTANCE)`,
   with `BRIGHTNESS_SCALE = 64.0` a new fixed shader constant (not user-
   exposed) chosen so the new defaults (`density=0.012, albedo=1.0`) land
   near the old default's brightness (`intensity=0.8`) - flagged as a
   starting point needing live-tuning, not an exact derivation. Applied
   consistently across all three consumers (raymarch, atlas, composite) so
   they stay in sync. Per explicit user decision this was a full replacement
   with no migration: old settings are gone outright, existing tuned values
   are not preserved. UI: `panel_as_volumetric_lighting.xml`'s "Sun/moon
   intensity" and "Distance extinction" sliders became "Scatter albedo"
   (0.00-1.00) and "Scatter density" (0.000-0.250).
3. **World-space density shaping.** Optional exponential height fog and
   animated 3-D density noise would prevent perfectly uniform shafts and make
   fog pool naturally. These require stable world/agent-space positions and a
   suitable repeatable 3-D noise resource; screen-space noise must not be used
   because it would stick to the camera. They are appearance features, not
   correctness fixes.
4. **True spotlight volumes.** Unity ray-intersects point-light spheres and
   spotlight cones separately, with cookies and available shadow maps. AS
   currently treats every selected local light as an unshadowed sphere. Cone
   intersection for viewer spotlights is transferable and would stop their fog
   glow outside the projected cone. Shadowed local volumetrics remain limited
   by the viewer's available spot-shadow maps; ordinary point lights still lack
   omnidirectional shadow maps.
5. **Sky-path policy.** Unity allows separate skybox extinction. AS caps all
   sky rays at 128 m. A deliberate sky coefficient could improve horizon
   continuity, but must agree with the viewer's existing atmospheric haze and
   avoid double fogging.
6. **Sun/moon treated as a point direction, not a disc.** Observed
   2026-08-22: god rays visibly converge toward a single point rather than
   radiating from across the sun's/moon's visible disc, confirmed via a
   moon screenshot with a treeline occluder clipping close to the visible
   convergence point.

   **First fix attempt (wrong, reverted in the same session):** evaluated
   `phaseHG` per-step against the existing shadow-sample disc jitter
   (`jitterDiscDirection`/`sample_light_dir`) instead of once against a
   single fixed `light_dir`. Rebuilt and retested with the jitter radius
   even exaggerated 4x (real ~0.53° -> ~2.1°) - **no visible change**, rays
   still converged to the same sharp point. Root cause of why this didn't
   work: per-pixel random jitter around a fixed center direction is just
   noise around that same center - it does not widen the angular region
   that reads as "bright," since the phase function already produces some
   falloff and jittering its input doesn't change where that falloff peaks
   on average. The real cause is that `phaseHG`'s falloff width (governed by
   `scatter_asymmetry`, tuned high at ~0.4-0.9 for a strong forward-scatter
   look) is much narrower than the disc's actual angular size, regardless of
   how the direction is sampled.

   **Second fix (correct, implemented):** clamp the angle fed into
   `phaseHG` so it can never read sharper than the disc's own angular
   radius - compute `raw_angle = acos(dot(ray_dir, light_dir))` once per
   pixel, then `disc_clamped_angle = max(raw_angle - SUN_MOON_ANGULAR_RADIUS, 0.0)`
   before taking its cosine and passing that into `phaseHG`. Every direction
   within the disc's radius of the true center now reads as angle-zero (the
   phase function's peak value), not just the exact center direction, so the
   bright region has real angular width matching the disc instead of always
   collapsing to a point irrespective of asymmetry. This `phase` value is
   computed once per pixel (it does not depend on shadow occlusion at all)
   and multiplies the final scatter output exactly as it did before this
   investigation began; the per-step shadow-sample disc jitter is unrelated
   and was left in place (it's still correct for soft shadow-edge penumbra,
   just was never the fix for this specific symptom).

   Also renamed `SUN_ANGULAR_RADIUS` -> `SUN_MOON_ANGULAR_RADIUS` (it was
   always used for both, name was misleading) and exaggerated its value 4x
   over the real ~0.53° (to ~2.1°, `0.0372` rad) for a more visible artistic
   effect.

   **Verification (temporary debug mode 12, added and removed within this
   session):** a raw phase-magnitude debug view (normalized against the
   asymmetry's own on-axis peak) confirmed the angle-clamp fix genuinely
   works at the phase level - the bright region has real, visibly soft
   angular width around the light's position, not a single point. This
   first required fixing a wiring bug in the debug view itself: mode 12 was
   initially gated out by `renderPass()`'s `if (debug_mode < 8)` raymarch
   dispatch (that check exists to exclude modes 8-11, which belong to
   local-light/atlas shaders, but 12 needs the raymarch shader to run) -
   the view read all-black at first purely because the shader containing it
   never executed, not because the fix was broken. Once let through, mode 12
   confirmed the fix works. Debug mode 12 has since been removed (shader,
   `renderPass()` dispatch, and XUI tooltip/`max_val`) now that its purpose
   - verifying this one fix - is done.

   `asVolumetricAtlasF.glsl` (the transparency atlas, used for
   transparent/foreground objects) has no disc-of-origin handling at all
   and was left untouched - it doesn't share this exact symptom today, but
   giving it the same angle-clamped-phase treatment would be a reasonable,
   still-open follow-up if foreground god-rays through glass/water ever show
   the same point-convergence look.

7. **Ray-shaft sharpness is shadow-map-bound, not phase-bound (open, found
   during item 6's verification).** Despite item 6's phase gradient now
   being correctly wide, normal mode 0 still shows sharp, narrow ray shafts
   converging to what looks like a point - because ray *shape/sharpness* in
   the final image is actually governed by `sampleDirectionalShadow`'s
   shadow-map occlusion boundary, not by the phase function's brightness
   falloff. The existing per-step disc jitter (`jitterDiscDirection`/
   `sample_light_dir`, used for the shadow lookup's surface-bias term) does
   not soften this: the shadow matrix used to reproject `sample_pos` into
   shadow-map space is still built from a single, un-jittered `light_dir`,
   so jittering the bias vector alone does not change which shadow-map
   texels actually get sampled - there is no real angular spread in the
   shadow lookup itself. Genuinely softening ray-shaft sharpness would need
   multi-tap shadow sampling across several angularly-offset light
   directions/matrices per march step (or an equivalent penumbra
   approximation) - a real cost/complexity increase, not a quick follow-up.

8. **Future feature: lens flare.** Not part of volumetric lighting proper -
   conceptually separate (screen-space lens-artifact overlay: internal
   reflections, aperture diffraction, anamorphic streaks keyed on the
   sun/moon's screen position and visibility) rather than a 3D participating-
   media raymarch. Should be its own new module (e.g. `ASLensFlare`), not
   folded into `ASVolumetricLighting` - different rendering technique,
   different shader stage, different failure modes. The one thing worth
   reusing: this module's raymarch already computes a sun/moon
   shadow-visibility/occlusion signal as a byproduct
   (`sampleDirectionalShadow`-based `mean_visibility`/`occlusion` in
   `asVolumetricLightF.glsl`) that a lens-flare pass could query rather than
   recomputing its own occlusion test from scratch. Needs its own scoping
   pass before implementation - not yet started.

Already present in AS under equivalent forms: bounded ray length, cascaded
directional-shadow sampling, Henyey-Greenstein phase, per-pixel march jitter,
depth-limited camera rays, additive HDR accumulation, and bounded point-light
volume intersection. These should not be reimplemented merely to resemble the
Unity source.

## GPU cost reductions with no visual change (2026-08-22)

Three optimizations were applied on top of the already-shipped atlas ping-pong
rewrite (which turned the 16-slice transparency atlas from an O(n^2) to an
O(n) shadow-sampling cost). All three are pure relocations or removals of
already-redundant work, not formula or blend-mode changes, so none of them
should alter a single output pixel.

1. **Local-light centers transformed once on the CPU instead of per-fragment
   on the GPU.** `asVolumetricLocalLightF.glsl` was computing
   `(modelview_matrix * vec4(local_light[i].xyz, 1.0)).xyz` inside the
   per-light loop of a full-screen fragment shader - identical work repeated
   for every screen pixel times every one of up to 64 candidate lights. Since
   each light's center is constant for the entire draw call, `renderLocalLights()`
   now transforms agent-space centers to view space once per light on the CPU
   (`LLViewerCamera::getInstance()->getModelview()`) and uploads the
   already-transformed `vec4`s. The shader's `modelview_matrix` uniform was
   removed as dead. Radius is unaffected since it is scale-invariant under a
   rotation+translation matrix.
2. **`sVolumetricTarget`'s FBO bind/flush moved inside `renderLocalLights()`,
   after its early-return checks.** Previously `renderPass()` unconditionally
   wrapped the call in `sVolumetricTarget.bindTarget()`/`flush()` even when
   `renderLocalLights()` was about to no-op (disabled setting, zero candidate
   lights, excluded debug mode) - `RenderVolumetricLocalLights` defaults off,
   so every normal frame was paying for a bind/flush pair around nothing. The
   bind/flush now happens only once the function has confirmed it has an
   actual draw to make.
3. **Removed the `sResolvedTarget` intermediate composite bounce buffer.**
   The normal (`debug_mode == 0`) composite path used to run two full-screen
   draws every frame: raymarch target -> `sResolvedTarget` (a full-resolution
   scratch copy), then `sResolvedTarget` -> `screen`. That intermediate
   existed solely so late water could separately sample and subtract the
   exact same resolved field from its own refracted framebuffer read (see
   the original `asVolumetricFull`/`fb.rgb -= texture(...)` mechanism). Water
   no longer does that - the "fixed water for real" pass (2026-08-22, see
   above) switched water to sampling the transparency atlas like every other
   transparent consumer, so `sResolvedTarget` had no remaining reader anywhere
   in the codebase. `renderPass()` now composites directly from
   `sVolumetricTarget` into `screen` in one draw; `sResolvedTarget`'s
   allocation, release, and completeness check were removed along with it.
   This is the largest of the three: it drops a full-resolution render target
   allocation, an extra FBO bind/flush pair, and a whole full-screen
   composite draw from every frame volumetrics are enabled, in the default
   (non-high-quality) case as well as high-quality.

None of these required a shader cache revision bump: the atlas F-shader's
math and output format are untouched by items 1-2, and item 3's shader
(`asVolumetricCompositeF.glsl`) was not modified at all - only which render
targets `renderPass()` wires into it changed.

### Transparency-atlas GPU cost reduction

The transparency-correct volumetric path originally built its 4x4 cumulative
depth atlas with one fullscreen draw. Although each atlas tile represented one
depth slice, the fragment shader independently recomputed every preceding
shadowed segment for that tile. The deepest tile therefore performed 16 shadow
samples per pixel, and the complete atlas performed an average of 8.5 segment
samples per atlas pixel every frame. This cumulative O(n²) work was identified
as the cause of substantially increased GPU load and fan noise.

The atlas construction is now incremental:

- `sTransparencyAtlas` is allocated at half the previous width and height,
  rounded up to multiples of four so all 16 tiles have exact integer bounds.
- `renderTransparencyAtlas()` issues 16 scissored draws, one per slice. Each
  draw samples only its new depth segment instead of repeating earlier shadow
  samples.
- Two full-atlas `GL_R16F` textures carry the raw, unclamped cumulative
  integral between slices. They ping-pong as the read source and MRT color
  attachment 1 because OpenGL forbids sampling the texture currently used as
  a render attachment.
- A hand-managed FBO attaches the final `GL_RGBA16F` atlas as color attachment
  0 and the current integral scratch texture as color attachment 1. The code
  saves and restores `LLRenderTarget`'s current FBO and resolution bookkeeping
  around this raw-GL section.
- The shader reconstructs tile-local screen UVs explicitly because scissoring
  clips fragments but does not remap the fullscreen triangle's interpolated
  `vary_fragcoord`. It samples the preceding slice from that slice's atlas-tile
  offset in the opposite ping-pong texture.
- `previous_slice_integral` uses the program's appended manual texture channel,
  leaving the link-assigned deferred and shadow sampler units undisturbed. An
  attempted change to reuse and explicitly unbind the sampler's link-assigned
  unit caused the main god-ray composite to disappear and corrupted later water
  rendering, so that change was reverted. If no appended unit is available,
  atlas rendering restores its shader, FBO, viewport, and render-target
  bookkeeping without drawing a partially valid cumulative atlas.

Every public atlas tile retains the existing consumer contract:
`light_color * clamp(raw_integral / MAX_MARCH_DISTANCE * phase * intensity)`.
The raw R16F scratch values are private to atlas generation; alpha, PBR alpha,
legacy material, and fullbright shaders continue to consume the RGBA16F atlas
as before. The R16F handoff does quantize the running sum between slices,
so equivalence is functional rather than bit-identical to the former single
fragment invocation.

Expected cost reduction comes from eliminating the cumulative redundant shadow
lookups and reducing atlas pixel throughput by approximately four times. The
tradeoff is 16 small draw submissions and two additional half-resolution
single-channel scratch textures.

This change is **not yet built or runtime-tested**. Before acceptance:

1. Clear the shader cache manually, build, and confirm that the atlas shader
   links with both fragment outputs and without GL/FBO errors in
   `AyaneStorm.log`.
2. Compare GPU utilization, frame time, temperature, and fan behavior against
   the preceding build in the same scene and camera position.
3. Inspect debug mode 10 and normal transparency for seams between atlas tiles,
   especially near viewport edges where bilinear filtering has the least
   inset at the new resolution.
4. Test Standard alpha, PBR alpha, legacy materials, fullbright alpha, Exact
   OIT, AVBOIT, and water at near, intermediate, and maximum atlas depths.
5. Resize the window through odd and even dimensions and confirm complete tile
   coverage, stable UV alignment, and correct resource reallocation.
6. Toggle volumetric lighting and shadow detail repeatedly and confirm no stale
   atlas contents, texture feedback warnings, or persistent GL-state changes.

Per the developer's active testing workflow, `shaderCacheRevision()` remains
at `v13`; the shader cache is cleared manually before each test build.

### Water flattening correction

Runtime comparison with volumetric lighting toggled showed that the
water-specific correction from commit `71d1838862` made the surface appear
nearly flat. That path subtracted `asVolumetricFull` from water's copied
framebuffer, completed ordinary reflection/refraction shading, then added a
separate smooth 16-step camera-to-water scatter march after all surface detail.
At long water-view distances the additive scatter dominated the waves,
reflection probes, refraction, and punctual highlights. Disabling the original
water RGB clamp exposed the oversized HDR term directly.

Moving the same foreground term into `fb` before the BRDF was runtime-tested
and remained flat: the smooth duplicate radiance still dominated wherever the
water equation retained a substantial refracted/background contribution, so
wave normals were calculated but visually overwhelmed.

The final correction removes the duplicate water-only estimator entirely.
Water no longer subtracts `asVolumetricFull` or performs another 16-step shadow
march. Its ordinary wave/Fresnel/reflection/refraction equation instead shades
the already-volumetric scene copied through `screenTex`. This is not a bypass:
volumetric radiance remains present in the scene being refracted, but it is no
longer added a second time as a smooth field unique to water. HDR output remains
enabled while volumetrics are active so the scene tonemapper, rather than the
legacy water clamp, handles that copied radiance.

A subsequent night/fog runtime test confirmed that this restored waves but
left water conspicuously dark: `screenTex` supplied volumetrics through the
refracted share, while the Fresnel-reflected share replaced it with dark probe
radiance. The targeted correction now samples the existing cumulative atlas at
the water surface depth and adds it only in proportion to `df2.x`, the same
Fresnel reflection weight used by the water BRDF (and shoreline `fade`). Thus
the refracted share retains its already-volumetric scene input and only the
missing reflected share receives camera-to-water scatter. This costs two atlas
lookups rather than restoring the removed 16-shadow-sample water march, and it
avoids the former full-strength post-BRDF overlay that erased wave contrast.

Runtime test passed: water wave and reflection detail remains visible, and the
surface now blends correctly with the volumetrically lit scene. The
Fresnel-weighted atlas correction is accepted as build-OK and runtime-tested
for the reported water regression (`bokt`).

## Prioritized performance and quality roadmap (2026-08-22)

This list reflects the current implementation after atlas O(n) accumulation,
Fresnel-weighted water integration, opaque transmittance, and removal of the
obsolete resolved target. Profile before and after each item; do not combine
multiple rendering changes into one runtime test.

### Priority 1: composite scatter and transmittance with destination blending

**Implemented 2026-08-22; awaiting build/runtime validation.** The composite
shader now outputs scatter in RGB and Beer-Lambert transmittance in alpha, and
the normal composite uses `ONE, SRC_ALPHA` destination blending. The
`sSceneCopyTarget` allocation, copy draw, sampler, and associated bandwidth
have been removed. Debug modes still replace the destination, while the alpha
write mask preserves the screen alpha used by later post-processing.

The previous opaque-transmittance path copied `screen` into
`sSceneCopyTarget`, then replaced `screen` with
`sceneCopy * transmittance + scatter`. In the default quality tier the copy
target is only half resolution, so this both adds a fullscreen copy/pass and
reconstructs the entire opaque scene from a lower-resolution texture. That is
an avoidable bandwidth cost and a potential base-scene quality regression.

The fixed-function blend unit can express the same RGB equation directly:

`C_out = C_src * ONE + C_dst * SRC_ALPHA`

The composite shader outputs scatter in RGB and per-pixel Beer-Lambert
transmittance in alpha, then uses separate RGB/alpha blending via
`blendFunc(BF_ONE, BF_SOURCE_ALPHA, BF_ZERO, BF_ONE)`. The existing color mask
preserves destination alpha. This removes `sSceneCopyTarget`, its RGBA16F
allocation, the screen-copy draw, the `sceneCopy` sampler, and the associated
texture bandwidth while retaining full-resolution destination detail. Debug
replace modes keep their existing non-blended behavior.

Expected result: the largest remaining low-risk performance win and a quality
improvement in the default half-resolution tier. Validate exact disabled
bypass, screen alpha, sky fade-to-unity, opaque transmittance, transparency,
water, and post-tonemap output before acceptance.

### Priority 2: distance-proportional directional sample count

**Implemented 2026-08-23; awaiting build/runtime validation.** The directional
raymarch now scales its active step count with the depth-limited ray length,
using a four-step floor and retaining the configured 16/32 steps at the 128 m
march limit. The physical step length and Beer-Lambert integral remain derived
from the actual active step count, so brightness does not intentionally change.

The previous directional shader executed the full configured 16 or 32 shadow
samples even when opaque depth ends the ray only a few metres from the camera.
The implemented count keeps approximately constant sample spacing instead:

`steps = clamp(ceil(sample_count * ray_len / 128), 4, sample_count)`

A minimum of four samples protects near-field stability. This can
substantially reduce shadow lookups in interiors and scenes dominated by nearby
geometry, with little reason to spend 16 samples over a two-metre ray. Because
sample count affects noise and shadow-transition stability, test camera motion,
near avatars, thin occluders, and debug visibility modes before shipping.

### Priority 3: cheaper sun/moon disc sampling

**Implemented 2026-08-23; awaiting build/runtime validation.** Static review of
the shared `sampleDirectionalShadow(pos, norm, pos_screen)` implementation
showed that its second argument is a surface normal, not a sampleable light
direction. Moreover, `pcfShadow()` does not use that normal argument; changing
it only introduced a tiny surface-bias offset in the wrapper. The former
per-step "disc jitter" therefore never moved the shadow-map lookup at all.

The ineffective work has been removed rather than replaced with a constant
disc sequence: two noise evaluations, `sqrt`, `sin`, `cos`, basis construction,
and normalization are gone from every active march step. Empty-space samples
now pass the actual light direction as their normal, producing the intended
zero surface-bias offset. The once-per-pixel march-position jitter, built-in
five-tap shadow PCF, and phase-function sun/moon disc width remain unchanged.

Every directional march step previously generated two gradient-noise values,
evaluates `sqrt`, `cos`, and `sin`, builds a tangent basis, and normalizes a
jittered light direction. The shadow lookup remains the dominant operation,
but this arithmetic is repeated at every pixel and step.

A constant low-discrepancy disc sequence would only be meaningful with a new
shadow sampler capable of perturbing the light-space lookup itself. Adding such
a sampler is a separate quality feature with cascade, bias, and stability risk;
it is not required for this removal of provably ineffective arithmetic.

### Floater visibility persistence (2026-08-23)

The standalone Volumetric Lighting floater already used `save_rect="true"`,
which restores its position but not its open/closed state. Adding
`save_visibility="true"` lets the registered-floater startup path restore the
previous visibility at login, matching other persistent utility floaters.

### Priority 4: temporal accumulation as an optional quality mode

**Deferred by decision on 2026-08-23.** The viewer has no active temporal
velocity/history pipeline to reuse (`SMAA_REPROJECTION` is disabled). A robust
implementation would require new history targets, depth/motion rejection,
camera-cut and lighting reset rules, and another fullscreen pass. That scope
and ghosting risk are not justified for the current stable path.

Reproject and blend the previous directional-scatter frame using motion/depth
rejection, allowing fewer current-frame shadow samples for similar apparent
quality. This has the highest potential quality-per-sample gain but also the
highest implementation and regression risk: camera cuts, animated foliage,
moving avatars, changing sun direction, water, and disocclusion can ghost.
Keep it optional, reset history aggressively, and do not make it a prerequisite
for the stable non-temporal path.

### Priority 5: improve the depth-aware upsample guide

**Implemented 2026-08-23; awaiting build/runtime validation.** The
half-resolution final composite now multiplies its existing relative-depth
weights by smooth normal-similarity weights. Invalid/background normals fall
back to depth-only weighting. The normal attachment is bound only when the
depth-aware upsample is active, leaving full-resolution and debug composites
unchanged.

The previous four-tap upsampler rejected samples using relative depth only. For
opaque geometry, adding a normal-similarity term can better preserve creases
and adjacent surfaces at similar depth, reducing light bleed without a blur.
This costs additional G-buffer samples and should be limited to the default
half-resolution tier. Transparency is rendered later and has its own atlas, so
the earlier alpha/hair objection to an opaque-depth bilateral final composite
does not apply in the same way here; nevertheless test foliage silhouettes and
thin geometry carefully.

### Priority 6: conditional atlas production

The 16-slice atlas is built every enabled frame even if no atlas-consuming
transparent geometry or water is ultimately visible. A conservative previous-
frame visibility flag from alpha/material/water submission could skip atlas
generation after consecutive unused frames, with immediate reactivation and a
one-frame-safe fallback when a consumer appears. This is attractive in opaque
scenes but invasive because incorrect visibility prediction would cause stale
or missing transparency lighting. Attempt only after GPU profiling shows the
optimized atlas remains material.

### Lower-priority or avoid

- Local-light fog defaults off. Its CPU-transform and conditional-FBO changes
  already remove common-path waste; tiled light culling or instanced light
  volumes are not justified unless profiling with the option enabled shows a
  real bottleneck.
- Do not restore a generic bilateral blur. It adds multiple passes and repeats
  previously documented transparency/guide problems.
- Do not increase default atlas resolution or slice count before profiling the
  current half-resolution 16-slice result. Prefer gutters/inset corrections
  only if runtime captures show actual tile bleeding.
- Dynamic resolution based on measured GPU time is useful eventually, but it
  changes visual sharpness during play and needs hysteresis. First expose or
  profile fixed quarter/half/full tiers to establish a stable cost curve.
