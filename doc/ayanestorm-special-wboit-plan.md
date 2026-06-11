# AyaneStorm-Special: Weighted Blended OIT (WBOIT) Implementation Plan

**Author**: AS:Chanayane  
**Date**: 2026-06-09  
**Branch**: `special-ayanestorm-dev`  
**Status**: Implemented and runtime-tested; WBOIT is optional and current remaining issue is self layered-hair opacity/intersection artifacts.

---

## Current Runtime State (2026-06-11)

WBOIT is now behind `RenderWBOIT`, exposed in Preferences > AyaneStorm as `Use improved transparency rendering (WBOIT)`.

Latest architecture after transparency bug review:
- WBOIT is split into two accumulation/composite layers.
- First layer: world/sim alpha accumulates into WBOIT, then composites over the opaque scene.
- Second layer: WBOIT is cleared/reused, avatar/attachment alpha accumulates into WBOIT, then composites over the already-composited world result.
- The post-WBOIT legacy pass is narrowed to custom blend modes that WBOIT cannot represent directly.
- The earlier self/other rigged post-WBOIT split is no longer the intended direction because it made self layered hair mostly exercise unsorted legacy alpha instead of WBOIT.
- Full legacy fallback for avatar attachments was considered and rejected because it would likely restore the vanilla bug where hair/lashes erase sheer worn layers behind them.
- A three-layer rigged-avatar vs non-rigged-attachment split was tested and removed after it made a beard attachment render in front of rigged long hair.

When `RenderWBOIT` is disabled, the post-water alpha dispatch intentionally matches reference commit `6d68bc063cc110258851b0f2a03596badb11b73e`, before:
- `82e5ea45c680e028c181fe551b7758a9d8b343bc` (`fix alpha blend inspired from the work of Mayatonton`)
- `a90c1319555c60cf2702bf139365d3659ec9c8f0` (`Weighted Blended Order-Independent Transparency`)

Runtime-validated improvements:
- Transparent objects are visible again.
- PBR/GLTF-looking transparent objects, including a glass panel, are visible again.
- Hair/glass behavior is much improved compared with the initial WBOIT implementation.
- The black/white frame on switching 1st/3rd person is reduced to almost invisible.
- Other avatars' hair no longer appears in front of the user's hair after the post-WBOIT rigged split.
- The user's back/skin no longer appears through long hair after reverting the failed self-depth-prepass experiment.

Current known issue:
- The user's own layered hair can still look too transparent with WBOIT enabled. The user specifically reports seeing pavement/head/background through hair where hair strand texels should visually block the background.
- Multiple opacity and weight tuning attempts were not sufficient. Stronger attachment alpha promotion darkened makeup/hair/eyelashes without fixing the perceived transparency.
- A self rigged high-alpha depth prepass was tested and rejected because it made hair much worse.
- The two-layer WBOIT split tested much better for hair solidity and glass interaction, including the eyelashes-in-front-of-glass case, but avatar-side alpha can now look too dark.

Debug/runtime settings currently relevant:
- `RenderWBOIT`: enables/disables WBOIT.
- `RenderWBOITDebugTint`: temporary diagnostic, default off. When enabled, all WBOIT fragments become magenta/opaque, proving the edited WBOIT shaders are active.
- `RenderWBOITAttachmentAlphaBoost` was removed after testing. The subtle `0.35..0.85` coverage promotion did not fix hair transparency, and stronger variants made makeup/hair/eyelashes too dark.

---

## Implementation Status

### Fresh Handoff Notes

For a new conversation, start from these facts:

- The current code has accumulated many experimental changes. Check the working tree diff before making new edits.
- The latest architectural change is the two-layer WBOIT split controlled by `LLDrawPoolAlpha::sWBOITAvatarLayer`.
- World alpha is rendered by `LLDrawPoolAlpha::renderPostDeferred()` with `sWBOITAvatarLayer == false`, which currently calls `forwardRender(false, ATTACHMENT_NONE)`.
- Avatar/attachment alpha is rendered by rerunning the alpha pool from `LLPipeline::renderDeferredLighting()` with `sWBOITAvatarLayer == true`, which currently calls `forwardRender(true, ATTACHMENT_ALL)` and `forwardRender(false, ATTACHMENT_ONLY)`.
- WBOIT composite is now wrapped in a local `composite_wboit` lambda in `pipeline.cpp` so it can run once after world alpha and again after avatar/attachment alpha.
- Custom blend alpha remains in `ATTACHMENT_POST_WBOIT_LEGACY` after both WBOIT composites.

Do not repeat these failed approaches without a new hypothesis:

- Coarse rigged/alpha depth-only prepass. It made hair much worse.
- Broad or aggressive alpha promotion for all avatar attachments. It darkened makeup, hair, and eyelashes without fixing the hair transparency complaint.
- Re-enabling material WBOIT `mFeatures.hasAlphaMask`. It regressed hair/eyelashes in front of glass.
- Global near-opaque/skinned foreground color-weight boosts. They helped some cutouts but made glass disappear behind hair/lashes.

Preserve these known-good or useful changes unless retesting says otherwise:

- Near-opaque `0.995..1.0` coverage promotion for WBOIT alpha. It improved cage/fence/vent-style cutout textures without breaking 1% transparent clothing.
- Glow sub-pass flushing/rebinding around WBOIT so bloom/glow still works.
- Invalid accum/reveal guards in WBOIT composite.
- GLTF double-sided bucket `continue` fix.

### §9 Implementation Checklist

- [x] **Step 1 — Shaders**
  - [x] `wboitWeight.glsl` — weight function utility (new)
  - [x] `wboitCompositeF.glsl` — fullscreen composite fragment shader (new)
  - [x] `alphaF.glsl` — `#ifdef WBOIT` output branch added
  - [x] `pbralphaF.glsl` — `#ifdef WBOIT` output branch added (non-HUD block only)
  - [x] `materialF.glsl` — `#ifdef WBOIT` output branch added (BLEND mode block only)
  - [x] `fullbrightF.glsl` — `#ifdef WBOIT` output branch added

- [x] **Step 2 — RT allocation**
  - [x] `pipeline.h` — `wboitFBO` added to `RenderTargetPack` (2×RGBA16F attachments, shared depth)
  - [x] `pipeline.cpp` — allocated inside `!gCubeSnapshot` block; released in `releaseScreenBuffers()`
  - Note: plan called for separate `wboitAccum`/`wboitReveal`; implemented as single `wboitFBO` with `addColorAttachment` to avoid `GL_R16F` pixel-transfer format limitation in `LLRenderTarget`

- [x] **Step 3 — Shader registration**
  - [x] `llviewershadermgr.h` — 8 new `extern LLGLSLShader` globals declared
  - [x] `llviewershadermgr.cpp` — globals defined, added to `mShaderList`, unloaded in `unloadShaders()`
  - [x] `gWBOITCompositeProgram` registered after `gDeferredPostProgram`
  - [x] `gDeferredAlphaWBOITProgram` + skinned variant
  - [x] `gDeferredPBRAlphaWBOITProgram` + skinned variant
  - [x] `gDeferredFullbrightAlphaWBOITProgram` + skinned variant
  - [x] `gDeferredMaterialAlphaWBOITProgram[SHADER_COUNT*2]` (BLEND slots only)
  - Note: `wboitWeight.glsl` exists as reference only. It is not linked as a separate shader object because that caused duplicate helper-symbol/linkage issues; the weight function is inlined in each WBOIT shader branch.

- [x] **Step 4 — Accumulation pass wiring**
  - [x] `lldrawpoolalpha.h` — `mForwardToWBOIT` flag added
  - [x] `renderPostDeferred` — WBOIT branch for POST_WATER non-impostor non-cubemap; prepares WBOIT shader variants; clears MRT; runs 3-pass dispatch; restores screen RT
  - [x] `forwardRender` — `write_depth = false` when WBOIT; binds `wboitFBO` MRT; sets per-attachment blend via `glBlendFunci` (attachment 0 additive, attachment 1 multiplicative)
  - [x] `renderAlpha` — selects WBOIT shader variants when `mForwardToWBOIT`; emissive glow sub-pass suppressed under WBOIT
  - [x] GLTF/PBR alpha BLEND now routes to `gDeferredPBRAlphaWBOITProgram` under WBOIT instead of falling back to the standard PBR alpha shader
  - [x] Avatar/attachment/custom-blend alpha is skipped from WBOIT accumulation and handled by a post-WBOIT legacy pass

- [x] **Step 5 — Composite pass**
  - [x] `pipeline.cpp` — composite inserted after `renderGeomPostDeferred`, before `screen_target->flush()`
  - [x] Guarded by `RenderWBOIT && !gCubeSnapshot && !sImpostorRender && LLDrawPoolAlpha::sWBOITRendered`
  - [x] Blend: `BF_SOURCE_ALPHA / BF_ONE_MINUS_SOURCE_ALPHA`
  - [x] Textures bound through `gWBOITCompositeProgram.bindTexture(...)`
  - [x] `LLShaderMgr::DEFERRED_DIFFUSE` → accum (attachment 0), `DEFERRED_SPECULAR` → reveal (attachment 1)

- [~] **Step 6 — Runtime validation**
  - [x] Transparent objects visible again
  - [x] Hair/glass behavior substantially improved
  - [x] PBR/GLTF-looking glass panel visible
  - [x] WBOIT preference checkbox visible and useful for comparing on/off
  - [x] WBOIT off path restored to reference commit `6d68bc0` dispatch
  - [x] Other avatars' hair no longer renders in front of the user's hair
  - [ ] Self layered rigged hair still needs a better opacity/order strategy
  - [ ] HUD, PRE_WATER, impostors, and cube snapshots/reflection probes still need explicit regression checks

---

## §1 Problem Statement

The 3-pass dispatch fix (`lldrawpoolalpha.cpp`) correctly orders:
- pass 1: SIM-rezzed non-rigged alpha BLEND (background windows, foliage)
- pass 2: all rigged alpha BLEND (hair, clothing) — writes depth
- pass 3: attachment non-rigged alpha BLEND (eyelash prims, prim accessories)

However, a structural problem remains: **rigged hair in pass 2 writes depth unconditionally** (`write_depth = rigged`), including for fully or nearly transparent hair fragments. This means:

- Eyelash prims (pass 3) behind the hair are depth-rejected even where the hair texture is transparent
- More generally: any chain of alpha BLEND surfaces (foliage → far hair → eyelashes → near hair) has incorrect compositing because depth test is order-independent but alpha BLEND is order-dependent

The correct solution is **Weighted Blended Order-Independent Transparency (WBOIT)** — McGuire & Bavoil 2013.

---

## §2 Algorithm Overview (WBOIT)

Instead of sorting and drawing alpha BLEND surfaces back-to-front with depth writes, WBOIT:

1. **Accumulation pass**: render ALL alpha BLEND geometry (unsorted) into two render targets:
   - `wboitFBO` attachment 0 (RGBA16F): weighted premultiplied color sum
   - `wboitFBO` attachment 1 (RGBA16F): reveal/transmittance value stored in `.r`

   Per-fragment, the fragment shader computes:
   ```glsl
   float w = clamp(pow(min(1.0, a * 10.0) + 0.01, 3.0) * 1e8 *
                   pow(1.0 - gl_FragCoord.z * 0.9, 3.0), 1e-2, 3e3);
   // accum target:
   frag_data[0] = vec4(color.rgb * a, a) * w;
   // reveal target, sampled from .r by composite:
   frag_data[1] = vec4(a);
   ```
   (standard McGuire 2013 weight function — biased toward near, opaque-leaning fragments)

2. **Composite pass**: fullscreen quad reads `mWBOITAccum` and `mWBOITReveal` and composites over the opaque scene:
   ```glsl
   vec4 accum = texture(uAccum, uv);
   float reveal = texture(uReveal, uv).r;
   vec3 average_color = accum.rgb / max(accum.a, 1e-5);
   frag_color = vec4(average_color, 1.0 - reveal);
   // then standard "over" blend onto opaque scene
   ```

**Key properties**:
- No sorting required
- Single extra pass (accumulation) + one fullscreen composite
- Correct for non-overlapping or weakly-overlapping alpha surfaces
- Approximate for strongly overlapping same-depth surfaces (acceptable for SL hair/clothing)
- Depth write OFF for all alpha BLEND — no more depth rejection through transparent fragments

**Known limitation**: Not physically exact for complex interleaved alpha (e.g. many same-avatar hair cards at similar depth). Runtime testing confirms this is still the main unresolved issue for the user's own layered hair.

---

## §3 Scope and Non-Goals

### In scope
- All alpha BLEND surfaces currently going through `LLDrawPoolAlpha::renderAlpha`:
  - Legacy Blinn-Phong alpha (`alphaF.glsl`)
  - PBR alpha (`pbralphaF.glsl`)
  - Fullbright alpha (`fullbrightF.glsl`)
  - Legacy material alpha (`materialF.glsl`)
  - Both rigged and non-rigged variants
  - Both attachment and SIM-rezzed geometry
  - POST_WATER pass (primary scene)

### Out of scope / special handling
- PRE_WATER pass (water fog requires existing depth logic)
- HUD pass (forwardRender called once, separate pipeline)
- Opaque / alpha MASK geometry (deferred gbuffer path unchanged)
- The original pre-WBOIT path must remain available through `RenderWBOIT = false`
- Avatar/attachment/custom-blend alpha is no longer pure WBOIT; it uses a hybrid post-WBOIT legacy path because WBOIT averaged foreground hair/eyelashes into background glass too aggressively
- DoF, SSAO, SSR integration with WBOIT targets (separate future chapter)
- Impostor render path (keep existing write_depth logic for impostors)
- Cube snapshot path (keep existing logic)

---

## §4 Files to Change

### 4.1 New files (shaders)
- `indra/newview/app_settings/shaders/class1/deferred/wboitWeight.glsl`
  - Reference utility only; not linked as a separate shader object
  - The actual weight function is inlined in WBOIT shader branches

- `indra/newview/app_settings/shaders/class1/deferred/wboitCompositeF.glsl`
  - Fullscreen composite fragment shader
  - Inputs: `uniform sampler2D diffuseRect` for accum and `uniform sampler2D specularRect` for reveal
  - Output: `frag_color` for standard alpha blending over the opaque scene

### 4.2 Modified files (shaders)
The accumulation pass needs the same lighting computation as the existing alpha shaders, but with a different output. The cleanest approach is **not** to fork the entire shader, but to add a `#define WBOIT` path at the output stage only.

Files to add `#ifdef WBOIT` output branch:
- `indra/newview/app_settings/shaders/class2/deferred/alphaF.glsl`
  - Current output: `frag_color = max(color, vec4(0));`
  - Add at end: `#ifdef WBOIT ... outAccum / outReveal writes ... #else frag_color = ... #endif`
- `indra/newview/app_settings/shaders/class2/deferred/pbralphaF.glsl`
  - Same pattern, two `frag_color =` lines (one per `#if` branch in the file)
- `indra/newview/app_settings/shaders/class3/deferred/materialF.glsl`
  - Same pattern
- `indra/newview/app_settings/shaders/class1/deferred/fullbrightF.glsl`
  - Same pattern

**Implemented note**: The first attempt used explicit layout outputs and a float reveal target. Runtime debugging switched this to the viewer MRT convention:
```glsl
out vec4 frag_data[2];
```
and reveal is written as `frag_data[1] = vec4(alpha)`.

Each alpha shader also needs the weight function. Add it as a shared include:
- `indra/newview/app_settings/shaders/class1/deferred/wboitWeight.glsl` — NEW utility include
  ```glsl
  float wboit_weight(float a, float depth) {
      return clamp(pow(min(1.0, a * 10.0) + 0.01, 3.0) * 1e8 *
                   pow(1.0 - depth * 0.9, 3.0), 1e-2, 3e3);
  }
  ```

### 4.3 `indra/newview/pipeline.h`
Implemented in `RenderTargetPack` as a single combined target, not separate targets:
```cpp
LLRenderTarget wboitFBO; // attachment0 accum, attachment1 reveal
```

### 4.4 `indra/newview/pipeline.cpp`

**Implemented allocation**:
```cpp
mRT->wboitFBO.release();
if (!mRT->wboitFBO.allocate(resX, resY, GL_RGBA16F)) return false;
if (!mRT->wboitFBO.addColorAttachment(GL_RGBA16F)) return false;
mRT->deferredScreen.shareDepthBuffer(mRT->wboitFBO);
```

**Release** (in `releaseScreenBuffers` or wherever other RTs are released):
```cpp
mRT->wboitFBO.release();
```

**Composite pass** — insert in `renderDeferredLighting` (`pipeline.cpp`) immediately after
`renderGeomPostDeferred(*LLViewerCamera::getInstance())` (line ~9856) and before
`screen_target->flush()` (line ~9860). `mScreenTriangleVB` is a persistent member of
`LLPipeline` (allocated at line 559, released at line 763) — use the standard pattern
already used at lines 8827, 8852, 9810, etc.

```cpp
    // <AS:Chanayane> WBOIT composite — blend accumulated transparency over opaque scene
    if (render_wboit && !gCubeSnapshot && !sImpostorRender && LLDrawPoolAlpha::sWBOITRendered)
    {
        gGL.setColorMask(true, false);
        LLGLEnable blend(GL_BLEND);
        gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);

        gWBOITCompositeProgram.bind();
        gWBOITCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE,  &mRT->wboitFBO, false, LLTexUnit::TFO_POINT, 0);
        gWBOITCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR, &mRT->wboitFBO, false, LLTexUnit::TFO_POINT, 1);
        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gWBOITCompositeProgram.unbind();

        gGL.setColorMask(true, true);
        gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
    }
    // </AS:Chanayane>
```

Note: `LLShaderMgr::DEFERRED_DIFFUSE` and `DEFERRED_SPECULAR` are reused as convenient
texture slot names for the composite shader — the actual uniform names in
`wboitCompositeF.glsl` should match whatever slots are bound here.

### 4.5 `indra/newview/llviewershadermgr.h`
Declare:
```cpp
extern LLGLSLShader gWBOITCompositeProgram;
```

### 4.6 `indra/newview/llviewershadermgr.cpp`
Define and register `gWBOITCompositeProgram`:
```cpp
LLGLSLShader gWBOITCompositeProgram;
```
In `createDeferredShaders()`:
```cpp
gWBOITCompositeProgram.mName = "WBOIT Composite Shader";
gWBOITCompositeProgram.mShaderFiles.clear();
gWBOITCompositeProgram.mShaderFiles.push_back(
    make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
gWBOITCompositeProgram.mShaderFiles.push_back(
    make_pair("deferred/wboitCompositeF.glsl", GL_FRAGMENT_SHADER));
gWBOITCompositeProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
success = gWBOITCompositeProgram.createShader();
```
Add unload in `unloadShaders()`.

### 4.7 `indra/newview/lldrawpoolalpha.cpp`

**`renderPostDeferred`**: replace the 3-pass `forwardRender` block for POST_WATER with WBOIT-aware version:

```cpp
if (!LLPipeline::sRenderingHUDs && getType() == LLDrawPool::POOL_ALPHA_POST_WATER
    && !LLPipeline::sImpostorRender && !gCubeSnapshot)
{
    // <AS:Chanayane> WBOIT path
    // Clear combined MRT once per frame via raw GL:
    // attachment 0 accum = (0,0,0,0)
    // attachment 1 reveal = (1,1,1,1)
    // Bind wboitFBO MRT: [0]=accum, [1]=reveal, depth shared from deferredScreen
    mForwardToWBOIT = true;
    forwardRender(false, ATTACHMENT_NONE);
    forwardRender(true,  ATTACHMENT_ALL);
    forwardRender(false, ATTACHMENT_ONLY);
    mForwardToWBOIT = false;

    // restore screen RT for composite (done in pipeline.cpp renderDeferredLighting)
    gPipeline.mRT->screen.bindTarget();
    // </AS:Chanayane>
}
else
{
    // PRE_WATER / HUD / impostor / cube snapshot: keep existing path
    if (!LLPipeline::sRenderingHUDs) forwardRender(true);
    forwardRender();
}
```

**`forwardRender`**: when `mForwardToWBOIT == true`:
- Set `write_depth = false` for ALL passes (including rigged) — WBOIT does not write depth
- Set blend mode to additive accumulation:
  - `outAccum`: `glBlendFunc(GL_ONE, GL_ONE)` (additive)
  - `outReveal`: `glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR)` (multiplicative)
  - These are different per-attachment blend functions — requires `glBlendFunci` (OpenGL 4.0, available on this hardware per GLInfo)

**`renderAlpha`**: when WBOIT active, the shader bound must be the WBOIT variant (with `#define WBOIT`). This can be done by setting a define on the shader before binding, or by using a dedicated WBOIT shader variant registered separately.

Add to `LLDrawPoolAlpha`:
```cpp
bool mForwardToWBOIT = false;
```

---

## §5 MRT Binding Strategy

Binding two separate `LLRenderTarget`s as MRT attachments 0 and 1 requires either:
- **Option A**: A combined FBO that has both color attachments attached. Implemented as `LLRenderTarget wboitFBO` with attachment 0 for accum and attachment 1 for reveal, sharing depth from `mRT->deferredScreen`.
- **Option B**: Manual `glDrawBuffers` calls after binding. More fragile.

**Implemented: Option A.** `wboitFBO` is the combined binding target. Both color attachments are RGBA16F, with reveal stored in the red channel.

---

## §6 Shader `#define WBOIT` Injection

`LLGLSLShader` supports defines via `mDefines` (a `std::map<std::string, std::string>`). The WBOIT shader variants can be created by:
1. Registering separate `LLGLSLShader` objects (e.g. `gDeferredAlphaWBOITProgram`) with `WBOIT` in their defines map
2. Or dynamically recompiling with the define (expensive, avoid)

**Recommended: Option 1** — register dedicated WBOIT variants for each alpha shader:
- `gDeferredAlphaWBOITProgram` (from `alphaF.glsl` + `#define WBOIT`)
- `gDeferredAlphaWBOITProgram.mRiggedVariant` (rigged variant, same define)
- `gDeferredPBRAlphaWBOITProgram` (from `pbralphaF.glsl` + `#define WBOIT`)
- `gDeferredFullbrightAlphaWBOITProgram` (from `fullbrightF.glsl` + `#define WBOIT`)
- `gDeferredMaterialAlphaWBOITProgram[SHADER_COUNT*2]` (from `materialF.glsl` + `#define WBOIT`)

In `renderAlpha`, when `mForwardToWBOIT == true`, select the WBOIT variant instead of the standard shader.

---

## §7 Interaction with Existing Systems

### 3-pass dispatch / WBOIT-off behavior
The Mayatonton 3-pass attachment split is now guarded by `RenderWBOIT`. When WBOIT is disabled, post-water alpha dispatch reverts to reference commit `6d68bc0`: rigged pass first when not rendering HUDs, then one regular alpha pass.

When WBOIT is enabled, the WBOIT accumulation path uses the attachment filters, while avatar/attachment/custom-blend alpha is excluded from WBOIT and drawn after composite in a hybrid legacy pass.

### Post-WBOIT avatar handling
Current best-so-far behavior:
- Other avatars' rigged alpha renders first with depth writes enabled.
- Self rigged alpha renders second with depth writes disabled.
- Non-rigged attachment/custom-blend alpha renders last with depth writes disabled.

Rejected experiment:
- A self rigged high-alpha depth prepass caused the user's transparent hair parts to become almost fully transparent and exposed back/skin again.

### DoF depth prepass (lines 219–236 in `renderPostDeferred`)
The DoF depth prepass (`gDeferredFullbrightAlphaMaskProgram` with `minimum_alpha=0.33`) runs **after** the WBOIT accumulation passes and **before** the composite. It should remain unchanged — it writes to `mRT->screen`'s depth buffer, which is correct.

### Impostor render (`LLPipeline::sImpostorRender`)
Keep existing path (`mForwardToWBOIT = false`). Impostors need depth writes.

### Cube snapshot (`gCubeSnapshot`)
Keep existing path. Cube snapshots render into `mAuxillaryRT`, WBOIT targets are only allocated for `mMainRT`.

### PRE_WATER
Keep existing path. Water fog requires depth-aware ordering.

### HUD
Keep existing path. HUD has its own pass.

---

## §8 Settings

WBOIT is user-configurable and enabled by default:

```xml
<!-- in app_settings/settings.xml -->
<key>RenderWBOIT</key>
<map>
  <key>Type</key><string>Boolean</string>
  <key>Value</key><integer>1</integer>
</map>
```

And wrap the WBOIT branch in `if (gSavedSettings.getBOOL("RenderWBOIT"))`.

Runtime behavior:
- `RenderWBOIT = true`: use WBOIT accumulation/composite plus the post-WBOIT legacy pass.
- `RenderWBOIT = false`: use the reference pre-`82e5ea45` / pre-`a90c1319` alpha dispatch from commit `6d68bc063cc110258851b0f2a03596badb11b73e`.
- UI: Preferences > AyaneStorm > `Use improved transparency rendering (WBOIT)`.

---

## §9 Implementation Order (recommended)

1. **Shaders first** — add `wboitWeight.glsl`, add `#ifdef WBOIT` output branch to `alphaF.glsl` and `pbralphaF.glsl`, write `wboitCompositeF.glsl`. Verify GLSL compiles in isolation.

2. **RT allocation** — add combined `wboitFBO` to `RenderTargetPack`, allocate two RGBA16F color attachments in `pipeline.cpp`, and share depth from `deferredScreen`. Verify no allocation failure at startup.

3. **Shader registration** — register WBOIT variants in `llviewershadermgr.cpp`. Verify link at startup.

4. **Accumulation pass** — wire `mForwardToWBOIT` flag in `lldrawpoolalpha.cpp`, bind MRT, set blend modes, use WBOIT shaders. At this point alpha surfaces should disappear (they go to accum RT, not composited yet).

5. **Composite pass** — add composite draw call in `pipeline.cpp` after alpha pool render. Alpha surfaces should reappear correctly.

6. **Validation** — current state:
   - Transparent objects visible again ✓
   - PBR/GLTF-looking glass panel visible ✓
   - Hair/glass behavior substantially improved ✓
   - Other avatars' hair no longer appears in front of self hair ✓
   - Self long/layered hair still shows excessive internal layers and visible card intersections ✗
   - HUD unaffected: explicit check still needed
   - PRE_WATER unaffected: explicit check still needed
   - Impostors unaffected: explicit check still needed
   - Cube snapshots/reflection probes unaffected: explicit check still needed

---

## §10 Key File Locations (this fork)

| File | Purpose |
|------|---------|
| `indra/newview/lldrawpoolalpha.cpp` | Alpha pool render dispatch (main change site) |
| `indra/newview/lldrawpoolalpha.h` | `AttachmentFilter` enum, `mForwardToWBOIT` flag |
| `indra/newview/pipeline.h` | `RenderTargetPack` struct — combined `wboitFBO` |
| `indra/newview/pipeline.cpp` | RT allocation + WBOIT composite pass location |
| `indra/newview/llviewershadermgr.h/.cpp` | Shader declarations and registration |
| `indra/newview/app_settings/shaders/class2/deferred/alphaF.glsl` | Legacy alpha shader — add `#ifdef WBOIT` output |
| `indra/newview/app_settings/shaders/class2/deferred/pbralphaF.glsl` | PBR alpha shader — same |
| `indra/newview/app_settings/shaders/class3/deferred/materialF.glsl` | Material alpha — same |
| `indra/newview/app_settings/shaders/class1/deferred/fullbrightF.glsl` | Fullbright alpha — same |
| `indra/newview/app_settings/shaders/class1/deferred/postDeferredNoTCV.glsl` | Reuse as vertex shader for composite |
| `indra/newview/llspatialpartition.h` | `LLDrawInfo::mAttachedToAvatar` (already added) |
| `indra/newview/llvovolume.cpp` | `mAttachedToAvatar` population (already added) |

---

## §11 Reference

- McGuire, Morgan and Louis Bavoil. "Weighted Blended Order-Independent Transparency." *Journal of Computer Graphics Techniques* 2.2 (2013). http://jcgt.org/published/0002/02/09/
- Existing 3-pass dispatch: `lldrawpoolalpha.cpp` lines 202–220 (POST_WATER branch)
- Existing RT allocation pattern: `pipeline.cpp` lines 964–984
- Existing composite pattern: `pipeline.cpp` line 8842 (`gDeferredPostProgram`)
- `glBlendFunci` (per-attachment blend): requires OpenGL 4.0 — confirmed available (GLVersion: 4.6.0 NVIDIA)
- Alpha shader output sites: `alphaF.glsl:317`, `pbralphaF.glsl:220,274`
