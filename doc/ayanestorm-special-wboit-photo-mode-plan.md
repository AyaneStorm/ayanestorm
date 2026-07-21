# AyaneStorm-Special: Photo Transparency Mode Plan

**Author**: AS:Chanayane  
**Date**: 2026-06-11  
**Status**: Proposal  
**Goal**: Add an optional high-quality transparency mode for still shots where reduced FPS is acceptable.

---

## 1. Problem Statement

Current two-layer WBOIT is a strong interactive solution:

- world/sim alpha is accumulated and composited first
- avatar/attachment alpha is accumulated and composited second
- custom blend alpha remains on the post-WBOIT legacy fallback

This already fixes many important ordering problems, but the remaining weakness is the same one seen throughout testing:

- dense layered avatar hair, eyelashes, and similar coverage-texture content are only approximated
- tuning WBOIT to make hair more solid can also make hair, makeup, and lashes too dark
- tuning it to look lighter can reintroduce background leakage or glass interaction regressions

For a dedicated photo mode, we can spend more GPU time to get better front-layer ordering for avatar transparency without replacing the whole transparency pipeline.

---

## 2. Recommended Direction

Implement a new optional **Photo Transparency Mode** using this hybrid strategy:

1. Keep the existing **world/sim WBOIT layer** unchanged.
2. Replace the current **avatar/attachment WBOIT layer** with **fixed-count front depth peeling**.
3. Keep the **post-WBOIT legacy fallback** for custom blend modes after the peeled avatar result is composited.

This keeps the proven world transparency improvements while spending extra work only where the current renderer still fails most visibly: avatar hair/lashes/makeup overlap.

---

## 3. Why This Architecture

### Why not full-scene depth peeling

- Too expensive in crowded scenes.
- Re-renders all transparent content many times.
- Throws away the fact that world alpha already improved under WBOIT.

### Why not per-pixel linked lists / A-buffer

- Much larger renderer project.
- Higher implementation and driver risk.
- Not a realistic short-branch feature compared with the current code shape.

### Why avatar-only peeling

- The main unresolved artifacts are avatar-centric.
- World glass, smoke, vents, fences, and reflective transparent objects already benefit from WBOIT.
- Limiting peeling to avatar/attachment alpha bounds cost and complexity.

### Why fixed-count peeling

- Predictable cost.
- Easier to expose as a photo-only option.
- Easier to debug than adaptive or open-ended peeling.

---

## 4. Proposed Runtime Modes

Add a new setting separate from `RenderWBOIT`.

Suggested settings:

- `RenderTransparencyPhotoMode` = `false` by default
- `RenderTransparencyPhotoPeelLayers` = default `4`

Behavior:

- `RenderWBOIT = false`, `RenderTransparencyPhotoMode = false`
  - Full legacy/reference behavior
- `RenderWBOIT = true`, `RenderTransparencyPhotoMode = false`
  - Current two-layer WBOIT behavior
- `RenderWBOIT = true`, `RenderTransparencyPhotoMode = true`
  - World/sim alpha uses current WBOIT
  - Avatar/attachment alpha uses fixed-count depth peeling
  - Custom blend alpha still uses legacy fallback after transparency composite

Photo mode should require `RenderWBOIT = true`. It is a refinement of the current WBOIT path, not a separate transparency architecture.

---

## 5. Rendering Design

### 5.1 World pass

Keep exactly the current path:

- accumulate world/sim alpha into WBOIT
- composite WBOIT world result over opaque scene

No photo-mode-specific changes here in the first version.

### 5.2 Avatar photo pass

Instead of reusing WBOIT for avatar/attachment alpha:

1. render the nearest avatar transparent layer
2. render the next avatar transparent layer behind it
3. continue for `N` peel layers
4. composite peeled layers front-to-back over the already composited world result

Scope for the first version:

- rigged avatar alpha
- non-rigged attachment alpha
- standard `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` only

Still excluded:

- custom blend alpha
- PRE_WATER
- HUD
- impostors
- cube snapshots / reflection probes

### 5.3 Legacy/custom blend pass

Keep the current post-WBOIT legacy fallback after the avatar photo composite:

- custom blend alpha remains on the existing legacy path

This avoids expanding the peel path to cases the current WBOIT path already treats as special.

---

## 6. Depth Peeling Model

Use **front-to-back fixed-count depth peeling** for the first implementation.

Each peel iteration needs:

- one depth texture containing the previous peeled front depth
- one color target for the current peeled layer
- depth test that rejects fragments not strictly behind the previous layer

Per iteration:

1. clear current peel color target
2. render avatar/attachment alpha with a peel shader variant
3. store nearest surviving depth for the current layer
4. composite that layer into an avatar accumulation target or directly over screen in strict front-to-back order

Recommended first implementation:

- peel into a dedicated avatar color target per layer
- composite each layer in order after it is rendered

This is simpler to reason about than trying to build a packed multi-layer buffer immediately.

---

## 7. Shader Strategy

Do not replace the existing WBOIT shaders. Add parallel peel variants.

### New shader variants

Add peel variants for:

- `alphaF.glsl`
- `pbralphaF.glsl`
- `materialF.glsl`
- `fullbrightF.glsl`

These variants should:

- shade exactly like the current alpha path
- output standard shaded color, not WBOIT accum/reveal
- discard fragments not belonging to the current peel layer

### Peel inputs

Uniforms will likely include:

- previous peel depth texture
- viewport size / texel size
- peel epsilon
- current peel index if needed for diagnostics

### Important rule

Do not mix WBOIT math into the peel shaders. The whole point is that the avatar photo mode should use ordered alpha compositing for the peeled layers.

---

## 8. Pipeline Integration

### 8.1 `pipeline.h`

Add render targets for photo mode, likely:

- avatar peel depth target(s)
- avatar peel color target

Exact layout can stay flexible, but the first implementation should optimize for clarity over minimum memory.

### 8.2 `pipeline.cpp`

Extend the current post-deferred transparency sequence:

1. render world/sim alpha via current WBOIT
2. composite world WBOIT
3. if photo mode is enabled:
   - run avatar/attachment peel pass loop
   - composite peeled avatar layers over world result
4. else:
   - run current avatar/attachment WBOIT layer
   - composite avatar WBOIT
5. run custom blend legacy fallback

This keeps the current architecture intact and swaps only step 3/4.

### 8.3 `lldrawpoolalpha.cpp`

Add a new forward path mode beside:

- normal alpha
- WBOIT accumulation
- post-WBOIT legacy fallback

Suggested new mode:

- photo peel avatar alpha

The batch filtering rules should remain consistent with the current two-layer split:

- world/sim alpha excluded from avatar photo pass
- avatar/attachment alpha included
- custom blend excluded

### 8.4 `llviewershadermgr.h/.cpp`

Register peel shader programs similarly to the current WBOIT variants.

Keep the variant count bounded:

- alpha peel
- PBR alpha peel
- fullbright alpha peel
- material alpha peel
- rigged variants as needed

---

## 9. Feature Staging

### Stage 1: Minimal viable photo mode

Goal:

- fixed `4` peeled avatar layers
- world remains current WBOIT
- custom blend remains legacy fallback

Hard constraints:

- no HUD support
- no PRE_WATER support
- no alpha-mask special behavior changes
- no build-time or runtime attempt to peel the full scene

Success criteria:

- hair over opaque background looks closer to vanilla than current WBOIT
- hair in front of glass remains better than legacy
- eyelashes do not erase or over-darken windows

### Stage 2: Configurable peel count

Add viewer setting for `2`, `4`, `6`, `8` layers.

Goal:

- let users trade quality for frame time in photo mode

### Stage 3: Hybrid overflow fallback

If needed later:

- peel first `N` layers
- accumulate deeper avatar transparency into a small avatar-only WBOIT remainder

This is optional and should not be in the first implementation.

---

## 10. Performance Expectations

Expected cost drivers:

- avatar alpha geometry is redrawn once per peel layer
- rigged hair and dense attachments are the most expensive cases
- PBR transparent avatars will be especially costly

This is acceptable for photo mode because:

- it is explicitly non-default
- still shots tolerate low FPS
- the scope is limited to avatar/attachment alpha, not the whole world

The first implementation should prefer correctness and debuggability over aggressive optimization.

---

## 11. Risks

### Technical risks

- peel depth precision and epsilon tuning
- same-depth or near-coplanar hair cards causing unstable layer selection
- integration with glow/emissive alpha behavior
- additional shader variant management complexity

### Behavioral risks

- visual mismatch between world WBOIT and avatar peeled transparency
- darker or brighter results if front-to-back compositing does not match the legacy alpha assumptions exactly
- unexpected edge cases with attachment PBR materials

### Project risks

- significantly larger implementation than another round of WBOIT tuning
- more render targets and more passes increase maintenance burden

---

## 12. Debugging and Validation Plan

Add temporary diagnostics similar to the current WBOIT debug workflow:

- tint each peel layer with a different debug color
- optional onscreen overlay showing peeled layer count used
- optional setting to freeze at a specific peel layer

Manual validation scenes should include:

- own layered hair over opaque pavement/background
- own hair crossing itself over head/skin
- eyelashes in front of glass windows
- hair in front of tinted glass
- hair in front of sheer worn clothing
- smoke/vents/fences to confirm the world WBOIT path still behaves as before

---

## 13. Recommended First Implementation Checklist

- [ ] Add `RenderTransparencyPhotoMode`
- [ ] Add `RenderTransparencyPhotoPeelLayers`
- [ ] Add avatar peel render targets
- [ ] Add peel shader variants for alpha / PBR alpha / material alpha / fullbright alpha
- [ ] Add peel-specific avatar/attachment dispatch in `lldrawpoolalpha.cpp`
- [ ] Branch current transparency sequence in `pipeline.cpp`
- [ ] Composite peeled avatar layers front-to-back over world result
- [ ] Preserve current custom blend legacy fallback
- [ ] Add debug tinting per peel layer
- [ ] Manually validate against current two-layer WBOIT

---

## 14. Recommendation

If this feature is pursued, the first version should be:

- **world/sim = current WBOIT**
- **avatar/attachment = fixed 4-layer depth peeling**
- **custom blend = current legacy fallback**

That is the highest-value photo-mode implementation that is still realistic in this codebase. It directly targets the remaining hair/lash quality problem without discarding the current WBOIT improvements or expanding into a full transparency renderer rewrite.
