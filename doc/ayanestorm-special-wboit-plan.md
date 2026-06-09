# AyaneStorm-Special: Weighted Blended OIT (WBOIT) Implementation Plan

**Author**: AS:Chanayane  
**Date**: 2026-06-09  
**Branch**: `special-ayanestorm-dev`  
**Status**: Implementation complete — pending build & validation

---

## Implementation Status

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
  - Note: `wboitWeight.glsl` linked as a separate `GL_FRAGMENT_SHADER` object alongside each alpha shader (multi-object linking, valid OpenGL)

- [x] **Step 4 — Accumulation pass wiring**
  - [x] `lldrawpoolalpha.h` — `mForwardToWBOIT` flag added
  - [x] `renderPostDeferred` — WBOIT branch for POST_WATER non-impostor non-cubemap; prepares WBOIT shader variants; clears MRT; runs 3-pass dispatch; restores screen RT
  - [x] `forwardRender` — `write_depth = false` when WBOIT; binds `wboitFBO` MRT; sets per-attachment blend via `glBlendFunci` (attachment 0 additive, attachment 1 multiplicative)
  - [x] `renderAlpha` — selects WBOIT shader variants when `mForwardToWBOIT`; emissive glow sub-pass suppressed under WBOIT
  - Note: GLTF PBR alpha (`gltf_mat`) falls back to standard `pbr_shader` under WBOIT (no depth write, but not weighted-accumulated); dedicated GLTF WBOIT variant is future work

- [x] **Step 5 — Composite pass**
  - [x] `pipeline.cpp` — composite inserted after `renderGeomPostDeferred`, before `screen_target->flush()`
  - [x] Guarded by `!gCubeSnapshot && !sImpostorRender`
  - [x] Blend: `BF_ONE_MINUS_SOURCE_ALPHA` over `BF_SOURCE_ALPHA` (premultiplied "over" onto opaque scene)
  - [x] Textures bound via `wboitFBO.bindTexture(0/1, channel, TFO_POINT)`
  - [x] `LLShaderMgr::DEFERRED_DIFFUSE` → accum (attachment 0), `DEFERRED_SPECULAR` → reveal (attachment 1)

- [ ] **Step 6 — Validation** *(build first)*
  - [ ] Clean compile — no shader link errors at startup
  - [ ] Eyelash prims visible through transparent rigged hair
  - [ ] Background (foliage/windows) visible through hair
  - [ ] Hair visible through other hair
  - [ ] HUD unaffected
  - [ ] PRE_WATER (underwater) unaffected
  - [ ] Impostors unaffected
  - [ ] Cube snapshots / reflection probes unaffected

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
   - `mWBOITAccum` (RGBA16F): weighted premultiplied color sum
   - `mWBOITReveal` (R16F): weighted transmittance product

   Per-fragment, the fragment shader computes:
   ```glsl
   float w = clamp(pow(min(1.0, a * 10.0) + 0.01, 3.0) * 1e8 *
                   pow(1.0 - gl_FragCoord.z * 0.9, 3.0), 1e-2, 3e3);
   // accum target:
   outAccum = vec4(color.rgb * a, a) * w;
   // reveal target:
   outReveal = a;
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

**Known limitation**: Not physically exact for complex interleaved alpha (e.g. two surfaces at exactly the same depth). Visually excellent for all typical SL cases.

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

### Out of scope (do not change)
- PRE_WATER pass (water fog requires existing depth logic)
- HUD pass (forwardRender called once, separate pipeline)
- Opaque / alpha MASK geometry (deferred gbuffer path unchanged)
- The 3-pass dispatch for pass ordering (keep it — it still controls emissive and debug draw order)
- DoF, SSAO, SSR integration with WBOIT targets (separate future chapter)
- Impostor render path (keep existing write_depth logic for impostors)
- Cube snapshot path (keep existing logic)

---

## §4 Files to Change

### 4.1 New files (shaders)
- `indra/newview/app_settings/shaders/class1/deferred/wboitAccumF.glsl` — NEW
  - Fragment shader for accumulation pass
  - Receives same inputs as `alphaF.glsl` / `pbralphaF.glsl` (lit color + alpha)
  - Outputs to `layout(location=0) out vec4 outAccum` and `layout(location=1) out float outReveal`
  - Weight function as in §2

- `indra/newview/app_settings/shaders/class1/deferred/wboitCompositeF.glsl` — NEW
  - Fullscreen composite fragment shader
  - Inputs: `uniform sampler2D uAccum`, `uniform sampler2D uReveal`
  - Output: `frag_color` with premultiplied alpha for blending over opaque scene

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

**Important**: The `#ifdef WBOIT` variant needs to declare:
```glsl
layout(location=0) out vec4 outAccum;
layout(location=1) out float outReveal;
```
instead of `out vec4 frag_color`. These declarations must be guarded by `#ifdef WBOIT` to avoid conflicting with the standard path.

Each alpha shader also needs the weight function. Add it as a shared include:
- `indra/newview/app_settings/shaders/class1/deferred/wboitWeight.glsl` — NEW utility include
  ```glsl
  float wboit_weight(float a, float depth) {
      return clamp(pow(min(1.0, a * 10.0) + 0.01, 3.0) * 1e8 *
                   pow(1.0 - depth * 0.9, 3.0), 1e-2, 3e3);
  }
  ```

### 4.3 `indra/newview/pipeline.h`
Add to `RenderTargetPack` struct (around line 724, after `deferredLight`):
```cpp
// <AS:Chanayane> WBOIT accumulation targets
LLRenderTarget wboitAccum;   // RGBA16F — weighted color+alpha sum
LLRenderTarget wboitReveal;  // R16F    — weighted transmittance
// </AS:Chanayane>
```

### 4.4 `indra/newview/pipeline.cpp`

**Allocation** (in `allocateScreenBufferInternal`, around line 979):
```cpp
// <AS:Chanayane> WBOIT targets — share depth with screen so WBOIT
// accumulation depth-tests against opaque geometry
if (!mRT->wboitAccum.allocate(resX, resY, GL_RGBA16F)) return false;
mRT->wboitAccum.shareDepthBuffer(mRT->screen);
if (!mRT->wboitReveal.allocate(resX, resY, GL_R16F)) return false;
mRT->wboitReveal.shareDepthBuffer(mRT->screen);
// </AS:Chanayane>
```

**Release** (in `releaseScreenBuffers` or wherever other RTs are released):
```cpp
mRT->wboitAccum.release();
mRT->wboitReveal.release();
```

**Composite pass** — insert in `renderDeferredLighting` (`pipeline.cpp`) immediately after
`renderGeomPostDeferred(*LLViewerCamera::getInstance())` (line ~9856) and before
`screen_target->flush()` (line ~9860). `mScreenTriangleVB` is a persistent member of
`LLPipeline` (allocated at line 559, released at line 763) — use the standard pattern
already used at lines 8827, 8852, 9810, etc.

```cpp
    // <AS:Chanayane> WBOIT composite — blend accumulated transparency over opaque scene
    if (!gCubeSnapshot && !sImpostorRender)
    {
        mRT->screen.bindTarget();
        gGL.setColorMask(true, false);
        LLGLEnable blend(GL_BLEND);
        gGL.blendFunc(LLRender::BF_ONE_MINUS_SOURCE_ALPHA, LLRender::BF_SOURCE_ALPHA);

        gWBOITCompositeProgram.bind();
        gWBOITCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE,  &mRT->wboitAccum,  LLTexUnit::TFO_POINT);
        gWBOITCompositeProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR, &mRT->wboitReveal, LLTexUnit::TFO_POINT);
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
    // Clear accum to (0,0,0,0) and reveal to (1)
    gPipeline.mRT->wboitAccum.bindTarget();
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    gPipeline.mRT->wboitReveal.bindTarget();
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // bind MRT: [0]=wboitAccum, [1]=wboitReveal, depth shared from screen
    // use GLenum draw_buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    // (implementation detail: may need LLRenderTarget::bindMultiple or manual FBO setup)
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
- **Option A**: A combined FBO that has both textures attached. Create a helper `LLRenderTarget mWBOITFBO` that attaches `wboitAccum` and `wboitReveal` as color attachments 0 and 1, sharing depth from `mRT->screen`. This is the cleanest approach.
- **Option B**: Manual `glDrawBuffers` calls after binding. More fragile.

**Recommended: Option A.** Add `mWBOITFBO` to `RenderTargetPack` as the combined binding target. Allocate it with `GL_RGBA16F` for attachment 0 and manually attach the `wboitReveal` texture as attachment 1 via `glFramebufferTexture2D`.

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

### 3-pass dispatch (keep)
The `ATTACHMENT_NONE` / `ATTACHMENT_ALL` / `ATTACHMENT_ONLY` filter still controls which batches are processed in which call. Under WBOIT, ordering doesn't affect correctness (that's the point), but keeping 3 passes ensures emissive accumulation and debug highlight still work correctly per-pass.

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

No new user-facing settings. WBOIT is always-on for the POST_WATER non-impostor non-cubemap path. If a kill-switch is desired for debugging, add:

```xml
<!-- in app_settings/settings.xml -->
<key>RenderWBOIT</key>
<map>
  <key>Type</key><string>Boolean</string>
  <key>Value</key><integer>1</integer>
</map>
```

And wrap the WBOIT branch in `if (gSavedSettings.getBOOL("RenderWBOIT"))`.

---

## §9 Implementation Order (recommended)

1. **Shaders first** — add `wboitWeight.glsl`, add `#ifdef WBOIT` output branch to `alphaF.glsl` and `pbralphaF.glsl`, write `wboitCompositeF.glsl`. Verify GLSL compiles in isolation.

2. **RT allocation** — add `wboitAccum`, `wboitReveal`, `mWBOITFBO` to `RenderTargetPack`, allocate in `pipeline.cpp`. Verify no allocation failure at startup.

3. **Shader registration** — register WBOIT variants in `llviewershadermgr.cpp`. Verify link at startup.

4. **Accumulation pass** — wire `mForwardToWBOIT` flag in `lldrawpoolalpha.cpp`, bind MRT, set blend modes, use WBOIT shaders. At this point alpha surfaces should disappear (they go to accum RT, not composited yet).

5. **Composite pass** — add composite draw call in `pipeline.cpp` after alpha pool render. Alpha surfaces should reappear correctly.

6. **Validation** — test the canonical problem cases:
   - Eyelash prims visible through transparent rigged hair ✓
   - Background (foliage/windows) visible through hair ✓
   - Hair visible through other hair ✓
   - HUD unaffected ✓
   - PRE_WATER (underwater) unaffected ✓
   - Impostors unaffected ✓

---

## §10 Key File Locations (this fork)

| File | Purpose |
|------|---------|
| `indra/newview/lldrawpoolalpha.cpp` | Alpha pool render dispatch (main change site) |
| `indra/newview/lldrawpoolalpha.h` | `AttachmentFilter` enum, `mForwardToWBOIT` flag |
| `indra/newview/pipeline.h` | `RenderTargetPack` struct — add WBOIT RT members |
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
