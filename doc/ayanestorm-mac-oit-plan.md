# Mac OIT (mode 4): OpenGL 4.1 order-independent transparency

## Context

Exact OIT and AVBOIT are compiled out on macOS (`asavboit.cpp` `#if LL_DARWIN` stubs) because Apple caps OpenGL at 4.1: no compute shaders, SSBOs, image load/store, or atomics. Translation layers (Zink/MoltenVK/KosmicKrisp, MGL) were rejected in `doc/ayanestorm-special-exact-oit-macos.md`. macOS users only have Standard or AYAstorm (CPU group sort).

Goal: a new transparency mode, "Mac OIT", whose quality is close to AVBOIT/Exact OIT and whose performance is decent on Apple Silicon. It uses only OpenGL 4.1 core, so it is also offered on Windows and Linux. The design is free; it does not reuse AVBOIT internals.

## Research findings (why this design)

- AVBOIT's structure is sound and portable in principle: (1) estimate transmittance-in-front per fragment, (2) additive pass accumulating `Σ c·w`, `Σ w`, `Σ od`, (3) normalized resolve `opaque·T + (Σcw/Σw)·(1−T)` (`asAVBOITVolumeC.glsl` pass 7). Only its implementation of (1) needs GL 4.3: a froxel volume built with `imageAtomicAdd`, compute integration/warp, and exact front-4 keys built with `imageAtomicMin` (`asAVBOITCaptureF.glsl:474`).
- AVBOIT's quality comes mostly from the **exact front layers** (A9 front keys): layers 0..3 get exact source-over weights `α·Π(1−α_k)`. The volume only weights deeper layers, relative to the last key.
- GL 4.1 core supports everything needed to rebuild both parts with **blending** instead of atomics:
  - Per-attachment blend equations/functions (`glBlendEquationi`, `glBlendFunci`, GL 4.0), so one pass can mix MIN and ADD attachments.
  - `GL_MIN` blending on `R32F` returns one operand bit-exactly. For **positive, finite, normal** floats the IEEE bit pattern orders the same as the uint pattern, so `MIN` on `uintBitsToFloat(key)` is `atomicMin(key)` without atomics.
  - Additive float blending on RGBA32F/RGBA16F for moment and color accumulation. MRT: 8 draw buffers on Apple.
  - `glBlitFramebuffer` (GL 3.0) or attaching the existing depth texture replaces `glCopyImageSubData` (GL 4.3). `layout(binding=)` (GLSL 4.20) is not used; samplers are set by uniform.
- **Exact layers by MIN-blend peeling:** pass 0 finds `key0 = min(key)`. Pass k reads `key_{k-1}` as a texture and MIN-blends only fragments strictly behind it, giving `key_k`. That is K fixed passes with no loop, occlusion query or readback. It avoids the unbounded depth peeling that was abandoned earlier: K is fixed and small, it does not need depth-buffer ping-pong, and the tail is approximated.
- **Tail transmittance by moments:** Moment-Based OIT (Münstermann et al., I3D/HPG 2018) stores power moments of optical depth over warped depth, accumulated purely additively. It reconstructs `T(z)` per fragment in closed form (Hamburger 4-moment bound). This replaces the 3D volume and compute integration with a per-pixel, full-resolution representation. It also removes AVBOIT's 8×8-cell averaging artifacts (hair block/moire, `doc/ayanestorm-oit-avboit-hair-flicker-regression-todo.md`). Because the front K layers are exact, moment error only affects light already attenuated by `Π(1−α_k)`.
- Rejected alternatives: per-pixel histogram in 8×RGBA16F MRT (64 B/px of blend traffic per fragment, too heavy on Intel/AMD Macs); Weighted-Blended OIT alone (no real ordering, below AVBOIT quality); framebuffer fetch/raster order groups (not exposed by desktop Apple GL; only logged by the probe); stochastic transparency (noise, needs MSAA/TAA).

## Algorithm

K = number of exact layers (setting, 1..4, default 3). Geometry passes: **K + 1**, the same order as AVBOIT's 4–5.

**Layer key** (`macoit_key()` in the capture terminal): `d = log2(dist/near)/log2(far/near)` (linearized from `gl_FragCoord.z`) is quantized to 22 bits, and alpha to 8 bits: `key = ((d22 << 8) | a8) + 0x00800000u`, stored as `uintBitsToFloat(key)`. The `+0x00800000` keeps values normal so denormal flush cannot break them, and the max stays below `0x7F800000`. Relative depth precision is about 1.7e-6, around 3 µm at 2 m, which is finer than AVBOIT's 24-bit window depth. Clear value is `FLT_MAX`. Fragments with α ≤ 1/255 are not layers. Layer identity compares depth bits only, which reproduces AVBOIT's "distinct depth" rule. The key is computed by the same program in every pass, so it is bit-identical across passes, as AVBOIT A9 already relies on.

**Pass 0 (keys + moments)**, full res, depth test against opaque depth, no depth write:
- att0 `R32F`, `GL_MIN`: key0.
- att1 `RGBA32F`, ADD: `od·(z, z², z³, z⁴)`, where `od = −ln(1−α)` and z is warped log depth in [−1,1].
- att2 `R32F`, ADD: `b0 = Σ od`.

**Pass k = 1..K−1 (peel)**: read `key_{k-1}`. Fragments with `depth ≤ depth(key_{k-1})` are discarded. Fragments behind it MIN-blend into att `R32F` → key_k. These passes are alpha-only, using the material shaders' existing early-return before lighting and shadow (the same spot as `alphaF.glsl:335`).

**Color pass**: read key_0..key_{K-1}, moments, b0. Per fragment:
- If it is layer j < K: `w = α·Π_{k<j}(1−α_k)` (exact).
- Otherwise (tail): `w = α·Π_{k<K}(1−α_k)·clamp(T_mom(z)/T_mom(z_{K-1}), 0, 1)`. This is the relative trick from AVBOIT A9, so key layers are not counted twice. Apply a self-occlusion correction: evaluate `T_mom` just in front of the fragment, using the MBOIT bias/overestimation terms.
- Outputs, ADD blended: RGBA16F `(c·w, glow)`, R16F `w`, R16F `od`. This matches AVBOIT's accumulation formats.

**Resolve**: a fullscreen fragment pass (no compute) onto `screen`. It computes `T = exp(−Σod)` and outputs `(Σcw/Σw)·(1−T)`, using `glBlendFuncSeparate` so the result is `opaque·T + transparent`. Glow is written like AVBOIT's pass 7. With ≤ K layers per pixel the result is exact, matching Exact OIT.

**Opaque depth + stencil**: `deferredScreen` depth is `GL_DEPTH_COMPONENT24` with no stencil (`llrendertarget.cpp:98`). The capture uses a private `DEPTH24_STENCIL8` target. One fullscreen pass copies opaque depth into it (writing `gl_FragDepth`, since `glBlitFramebuffer` rejects mismatched depth formats), and stencil is cleared in the same step.

## Performance design

Each optimization is a hardware fast path, never a heuristic that changes the image.

1. **Stencil fragment count gates every later pass.** Pass 0 runs stencil `INCR` (saturating) on surviving layer fragments. Non-layer fragments (α ≤ 1/255) `discard`, so they are not counted. Stencil is then an upper bound on layers per pixel. Peel pass k uses stencil test `count > k`, and the resolve uses `count > 0`. Peel, color and resolve passes write neither depth nor stencil, so drivers (and Apple's hidden-surface removal) can reject early even though material shaders `discard`. This matters because `early_fragment_tests` is GLSL 4.20 and not available at 4.10. On a typical frame most pixels have 0–1 layers, so peel passes cost almost only vertex work, and the resolve touches only covered pixels. Duplicate coplanar fragments only over-count, so the gate stays conservative and exact.
2. **Alpha-only early return** in key/peel passes, before lighting, shadow sampling and reflection probes (the same placement as the existing AVBOIT block at `alphaF.glsl:335`). Only the color pass pays for full material shading, which is also true of Standard.
3. **Per-fragment branching in the color pass**: exact-layer fragments never evaluate moments. Only tail fragments run the closed-form 4-moment reconstruction (about 50 ALU, no loops, no texture fetches beyond the pixel's own texels).
4. **One-texel reads per pixel**: all per-pixel data (keys, moments, b0) is read with `texelFetch` at `gl_FragCoord`. There is no filtering, neighbourhood or dependent reads.
5. **Bandwidth budget**: pass 0 is 24 B/px (R32F + RGBA32F + R32F), each peel is 4 B/px, and color is 12 B/px (AVBOIT's formats). That is about 44 B/px at K=3, around 91 MB at 1080p. Nothing is read back and there is no `glFinish` or CPU sync. The only readback is the one-shot startup self-test.
6. **Uniforms set once per pass per program**, not per draw, as in AVBOIT A3. Blend state is set per attachment once per pass. The per-draw hook re-applies blend only where `LLDrawPoolAlpha` scopes it per `forwardRender`, the same constraint AVBOIT documented.
7. **Measured, not assumed**: every pass gets its own `LL_PROFILE_GPU_ZONE`. Candidates after phase 2 profiling, only if the data shows the need: 16-bit moments (MBOIT quantization), and caching skinned vertices with transform feedback (GL 3.0) if rigged hair is vertex-bound across the K+1 passes.

## Code quality bar

- One self-contained module (`asmacoit.*`) and one capture terminal shader. Shared material shaders get only a declaration, an early return and a store call.
- Every constant (key layout, clear value, bias, stencil semantics) is defined once and documented where it is defined, with the math stated in comments (key ordering proof, weight formula, resolve identity).
- No dead A/B paths or diagnostic clutter in hot code. Debug modes live in the resolve only.

**Startup self-test** (one-shot at resource allocation, not per frame): draw points into R32F with MIN blend and into RGBA32F with ADD blend, then `glReadPixels` once. Verify MIN bit-exactness and float blending. On failure, log it and fall back to Standard, the same way as the Exact OIT fallback in `asexactoit.cpp:1487`. It also logs `GL_MAX_DRAW_BUFFERS` and the extension list, including framebuffer-fetch presence, for the record.

## Implementation

New files (ours, no ownership tags, LF, `chanayane@firestorm`):
- `indra/newview/asmacoit.h/.cpp`: `ASMacOIT`, API mirroring `ASAVBOIT` (`requested`, `loadShaders`, `registerShaders`, `unloadShaders`, `shaderCacheRevision`, `beginFrame`, `captureActive`, `captureCompleted`, `renderPostDeferredCapture`, `configureCapturedDrawIfActive`, `handleCapturedEmissives`, `configureGLTFCapturedDraw`, `gltfProgram`, `*AlphaShader`, `finishFrame`, `allocateResources`, `releaseResources`, `appendDiagnostics`). It is not compiled out on any platform. The shader clone helper follows `cloneCaptureShader`/`cloneCapturePair` (`asavboit.cpp:348-394`), with a `MACOIT` define. The pass driver follows `ASAVBOIT::renderPostDeferredCapture` (`asavboit.cpp:748`): same gating (post-water pool only; no HUD, impostor or cube snapshot), same `pool.forwardRender(true/false)` plus GLTF scene rendering per pass.
- Shaders in `app_settings/shaders/class1/deferred/`: `asMacOITCaptureF.glsl` (key, moments, weights, `macoit_store()`), `asMacOITEmissiveF.glsl`, `asMacOITPbrGlowF.glsl` (glow into color-pass alpha, skipped in key/peel passes like `ASAVBOIT::handleCapturedEmissives`), `asMacOITResolveF.glsl` and `asMacOITDepthCopyF.glsl` (both with `postDeferredNoTCV.glsl`), and `asMacOITIsolateDepthF.glsl` (isolate-mode depth, as with AVBOIT).

Edits to existing files (minimal, `<AS:Chanayane>` tags, original code kept commented):
- `asoitdispatcher.cpp`: add `TransparencyMode::MAC_OIT = 4`, extend the range check, and route every dispatcher entry point to `ASMacOIT` first when its capture is active. `refreshOrderIndependentAlphaState()` includes `ASMacOIT::requested()`, which covers `llspatialpartition.cpp:676`.
- Shared material shaders: `class2/deferred/alphaF.glsl`, `pbralphaF.glsl`, `class3/deferred/materialF.glsl`, `class1/deferred/fullbrightF.glsl`, `class1/gltf/pbrmetallicroughnessF.glsl`. Add `#elif defined(MACOIT)` beside the `AVBOIT` blocks: a forward declaration, an alpha-only early return for passes before color, and `macoit_store()`.
- `llviewershadermgr.cpp`: register, load, unload, and add the revision to the cache hash, next to the ASAVBOIT lines 484/627/1233/3247. This new revision string is the module's first, not a bump.
- `pipeline.cpp` (1069/1495) and `llappviewer.cpp` (4146): allocate, release, diagnostics.
- `llshadermgr.cpp`: no change. The `MACOIT` define is not in `oit_storage_shader`, so it keeps `#version 410` on mac.
- `CMakeLists.txt`: new sources.
- `settings.xml`: extend the `ASRenderOITMode` comment to add 4; add `ASRenderMacOITExactLayers` (S32, default 3) and `ASRenderMacOITDebugMode` (S32, 0). Debug modes: 1 = coverage, 2 = total T, 3 = exact-layer count per pixel, 4 = tail-weight share.
- `panel_preferences_ayanestorm.xml`, `floater_phototools.xml`: combo entry "Mac OIT" (name open to change), exact-layers spinner, and tooltips.
- `doc/`: on approval, copy this plan to `doc/ayanestorm-mac-oit-plan.md`, and keep all later findings in that one file.

Every new or edited shader is validated with `.glslang/bin/glslang.exe` on a scratch copy prefixed `#version 410 core`. This is the real target version, so any 4.2+ construct fails there.

## Phases

1. **Core (Windows first):** module, capture/peel/color/resolve shaders, alpha/PBR/fullbright/material paths, dispatcher mode 4, and settings. The user builds on Windows and compares against Exact OIT (ground truth) and AVBOIT on the known cases: hair over glass, sheer over sheer, pane with thickness, foliage, particles.
2. **macOS runtime:** self-test log, then M4 correctness and GPU zone timings (`LL_PROFILE_GPU_ZONE` per pass) against Standard. Tune K and the moment bias.
3. **Parity:** emissive/PBR glow, GLTF, self-light isolate depth, DoF (snapshots first, per project rule), debug modes, and Linux check.

## Verification

- glslang at `#version 410 core` for every touched shader, before handoff.
- Windows `bokt`: side-by-side screenshots of Mac OIT vs Exact OIT vs AVBOIT on the same scenes. With ≤ K layers the result must be pixel-close to Exact OIT. Check that mode switching is live, Standard is unchanged, resize and snapshot work, and the log contains the self-test result.
- macOS `bokt`: self-test passes, visuals match Windows, and per-pass GPU timings plus total frame time are compared with Standard at 1, 2 and 3 exact layers. The user decides the acceptable cost.
- Log check (`%APPDATA%\AyaneStorm_x64\logs\AyaneStorm.log`) for GL errors or the fallback message.

## Implementation record (2026-09-30, phase 1, unbuilt)

Author: chanayane@firestorm. Status: code complete for phase 1, all shaders
validated with glslang at `#version 400` and `#version 410 core` (the loader
emits 400 on macOS), libraries link-checked. Not built, not run.

### Deviations from the plan above, and why

1. **Tail moments are accumulated in the last peel pass, anchored at exact
   layer K-2, with a per-pixel `[anchor, farthest]` warp** instead of pass 0
   with a global log warp. The reference model showed the global warp cannot
   resolve hair strands millimetres apart in float32 (mean transmittance error
   0.12). Measured final-pixel error against exact sorted compositing
   (`python scripts/testing/macoit_reference.py simulate`, 1500 pixels, hair /
   hair behind 1-2 panes / hair before a pane / sheer layers):

   | K | tail          | mean   | p95    | p99    | max    |
   |---|---------------|--------|--------|--------|--------|
   | 4 | none (Π only) | 0.0311 | 0.1482 | 0.2290 | 0.2662 |
   | 4 | global warp   | 0.0202 | 0.1056 | 0.1829 | 0.2303 |
   | 4 | key0 anchor   | 0.0046 | 0.0211 | 0.0522 | 0.0811 |
   | 4 | **K-2 anchor**| 0.0039 | 0.0187 | 0.0347 | 0.0589 |
   | 3 | K-2 anchor    | 0.0099 | 0.0432 | 0.0870 | 0.1255 |

   MBOIT's defaults (overestimation 0.25, bias 5e-7) were also the best in a
   sweep. Consequence: K is 2..4 (default 4).
2. **Pass 0 writes one target**: key0 (MIN), `-depth` (MIN, giving the
   farthest depth) and nothing else. Every peel target carries `-depth`
   forward, so the last peel can always read the farthest depth from the
   texture it samples.
3. **Total optical depth is accumulated in the color pass** (as AVBOIT did),
   freeing the key targets' alpha channel for the tail's zeroth moment.
4. **One sampler in the material programs.** macOS exposes 16 fragment
   texture units and the alpha material programs use most of them. A merge
   pass packs keys, moments and per-pixel precomputation (Cholesky factors,
   the moment fraction at layer K-1, farthest depth) into one W x 3H RGBA32F
   texture. A 3D or array texture was rejected: Apple's GL_MAX_3D_TEXTURE_SIZE
   is 2048 and LLTexUnit has no 2D-array type. When every capture program
   leaves the last unit free, all of them sample the state from that unit and
   it is bound once per pass; otherwise each program's linker-assigned unit is
   bound per draw. The log line "Mac OIT capture programs use up to N of M
   texture units" reports which path was taken.
5. **No shared-shader edits.** The five material shaders already expose a
   weighted-OIT hook under the `AVBOIT` define (alpha-only early return unless
   `avboitRasterPass == 2`, then `avboit_store()`). Mac OIT programs define
   `AVBOIT` and `MACOIT` and link `asMacOITCaptureF.glsl`, which implements
   `avboit_store()`. Side effect: those programs get `#version 430/450` on
   Windows/Linux, harmless since the code is 4.00-clean.
6. **Resolve is ungated by stencil** so glow on fully transparent prims
   survives; it discards uncovered pixels itself. It also writes the isolate
   depth (self-lighting floater), so no separate isolate program exists.
7. **The blending self-test runs at the first capture**, not at pipeline
   allocation, because it draws with the pipeline's screen triangle.

### Files

New: `indra/newview/asmacoit.{h,cpp}`; shaders
`class1/deferred/asMacOIT{Capture,Moments,Emissive,PbrGlow,DepthCopy,Merge,Resolve,Probe}F.glsl`;
`scripts/testing/macoit_reference.py` (float32-emulated pipeline model, 9
unit tests, `simulate` survey).

Tagged edits inside existing `<AS:Chanayane>` blocks: `llglslshader.cpp`
(two libraries join the OIT capture-library list), `llviewershadermgr.cpp`
(register, cache key, unload, load), `pipeline.cpp` (allocate, release),
`llappviewer.cpp` (diagnostics). Also `asoitdispatcher.cpp` (mode 4 routing),
`CMakeLists.txt`, `settings.xml` (`ASRenderMacOITExactLayers`,
`ASRenderMacOITMomentBias`, `ASRenderMacOITDebugMode`), both OIT combo boxes.

### Costs

- Geometry passes: K + 1 alpha passes (5 at K=4), same order as AVBOIT.
  Passes other than color are alpha-only; peel passes are stencil-gated to
  pixels with more than k fragments. Fullscreen: depth copy, merge, resolve.
- Memory: about 100 B/pixel (keysEven, keysOdd, moments RGBA32F; state 3 x
  RGBA32F; DEPTH24_STENCIL8): 207 MB at 1080p, 594 MB at 3024x1964, 830 MB at
  4K. `MACOIT_MB` in the About diagnostics reports the live figure. Reducing
  this is a phase-2 candidate if measurements call for it.
- No per-frame CPU readback, fence or glFinish.

### Known limitations

- Draws with custom additive blend factors are composited as source-over,
  exactly as in AVBOIT (Exact OIT honours them).
- Layer alpha in keys is 8-bit; coplanar duplicates share one layer, as in
  AVBOIT.

### Phase 1 verification checklist (Windows first)

1. Log: "Mac OIT shaders loaded", "blending self-test passed", texture-unit
   line. No GL errors.
2. Mode 4 vs Exact OIT, same camera: hair, hair behind one and two panes,
   dress over under-garment, foliage, particles with and without glow, lamp
   glass glow, GLTF alpha.
3. Debug modes (`ASRenderMacOITDebugMode`): 3 should show orange (4 layers)
   and red (tail) on dense hair, blue/green elsewhere.
4. `ASRenderMacOITExactLayers` 2/3/4 live; mode switching live; resize;
   snapshots; isolate backdrop with transparent content; Standard unchanged.
5. Tracy GPU zones "Mac OIT keys/peel/merge/color/resolve" against Standard.
