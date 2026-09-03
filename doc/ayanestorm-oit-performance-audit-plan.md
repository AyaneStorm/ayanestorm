# Exact OIT and AVBOIT: Performance Audit and Implementation Plan

Audit date: 2026-09-03. Branch `ayanestorm-dev` at commit `b1e15e0882`.
Audited files (all read in full):

- `indra/newview/fsexactoit.{h,cpp}`, `fsoitdispatcher.{h,cpp}`, `fsavboit.{h,cpp}`
- `app_settings/shaders/class1/deferred/exactOIT{Capture,Composite,Emissive,PbrGlow}F.glsl`, `exactOITSortC.glsl`
- `app_settings/shaders/class1/deferred/avboit*.glsl`
- OIT hooks in `lldrawpoolalpha.cpp`, `llspatialpartition.cpp`, `pipeline.cpp`, `llvopartgroup.cpp`, shared alpha shaders

Nothing was built or run for this audit. Every gain below is an engineering
estimate, not a measurement. The plan therefore starts with a measurement step.

---

## 0. How to use this document

Rules the implementer must follow (from `AGENTS.md`):

- Do not build. The user builds. Tell the user which items need a build.
- `fsexactoit.*`, `fsavboit.*`, `fsoitdispatcher.*` and every `exactOIT*.glsl` /
  `avboit*.glsl` file were created by AyaneStorm (author `chanayane@firestorm`).
  Edit them freely with ordinary comments. No ownership tags there.
- Any edit to `ll*.cpp/.h`, `llvopartgroup.cpp`, `lldrawpoolalpha.cpp`,
  `alphaV.glsl`, `fullbrightV.glsl` etc. must be minimal and wrapped in
  `// <AS:Chanayane>` ... `// </AS:Chanayane>` with the original line kept
  commented.
- Never use mutating git commands.
- Do not bump `FSExactOIT::shaderCacheRevision()` / `FSAVBOIT::shaderCacheRevision()`
  per change. Bump each once at the end of a phase that changed shader source or
  buffer layouts, and tell the user, because a stale program-binary cache will
  otherwise load old shaders against new C++ buffer layouts.
- Item order below is the recommended implementation order. Each item is
  independent unless a "Depends on" line says otherwise.

Each item has: **Goal**, **Why** (evidence from code), **Change** (exact edits),
**Traps**, **Verify**, **Exactness** (does the image change?), **Gain** (estimate).

Terminology: "node" = one captured fragment (32 bytes). "list" = a pixel's
linked list of nodes. "K" = shallow-list threshold introduced in item E5.

### Status and corrections log (keep current)

| Date | Item | State |
|------|------|-------|
| 2026-09-03 | Phase 0, Phase 1 (E1, E2-A, E3, E11) | committed `b49aefce07` |
| 2026-09-03 | E4 | implemented; first version regressed to ~1 FPS with sprites because the control SSBO was host-mapped. Corrected text below; details in `ayanestorm-oit-e4-fence-stall-question.md` |
| 2026-09-03 | E5, E6 | implemented, unstaged |
| 2026-09-03 | E1 growth timing | mid-frame `glBufferData` crashed (GPU hang on garbage links). Growth now deferred to `beginFrame()`; E1 and E10b text corrected; details in `ayanestorm-oit-e1-e4-growth-race.md` |

Two plan defects were found by implementation so far, both mine: E4 mapped
the atomically written buffer, and E1 reallocated mid-frame. Both corrected
in place. When a later item contradicts a correction here, the correction
wins.

---

## 1. Findings summary

### Exact OIT (lossless, priority)

| # | Finding | Severity | Item |
|---|---------|----------|------|
| 1 | Every frame does a synchronous GPU→CPU readback (`glGetBufferSubData`) right after capture: the CPU stalls until the whole frame so far is done, serialising CPU and GPU. | High (global FPS) | E4 |
| 2 | Sprite/particle overflow: when node demand exceeds capacity the frame is rendered twice (capture + full vanilla), and if the safe VRAM cap is hit this repeats every frame. Growth is only reactive. | High (sprites) | E1 |
| 3 | Particles are always drawn a second time through the emissive shader (their vertex buffer always has `TYPE_EMISSIVE`), allocating a glow node per fragment even when glow is 0. Doubles sprite node traffic and atomics. | High (sprites) | E2 |
| 4 | Four global atomics per captured fragment (`atomicAdd` node counter, `atomicMax` list max, plus two image atomics). The two SSBO atomics hit one address from every fragment on screen: classic PPLL contention bottleneck. | High (sprites) | E7 |
| 5 | Sort pass count is `ceil(log2(max list))` fullscreen passes. Every pixel, even lists of 2 to 8 nodes, goes through link-rewriting merge passes plus a separate blend traversal. | High | E5, E6 |
| 6 | Composite writes every screen pixel even where no node exists. | Low | E5 |
| 7 | Per draw call: uncached `glUniform1ui` + hashed lookups + two cached uniforms. `oitGlow` is always 0 in the capture path. | Low-Med (CPU) | E3 |
| 8 | Compute sort path (`exactOITSortC.glsl`): off by default, merge stage uses 1-thread workgroups, block sort uses 1 lane of 64 to walk the list. Its two queue buffers (4 B/pixel each) are allocated even when the setting is off. | Med (memory, dead weight) | E9 |
| 9 | Initial node pool is `4 * width * height` nodes (265 MB at 1080p, 1.06 GB at 4K). Never shrinks. | Med (memory) | E10 |
| 10 | Full-resolution RGBA16F opaque copy every frame (16 MB at 1080p) plus its target. | Low-Med (memory, bandwidth) | E10 |
| 11 | Fidelity deviation: the DoF depth pass is skipped when capture completed (`lldrawpoolalpha.cpp:242`). | Fidelity (only with DoF on) | E12 |

### AVBOIT (approximate)

| # | Finding | Severity | Item |
|---|---------|----------|------|
| 1 | CPU builds a Z-bin table + sparse RMQ table every frame (65536 × 17 words = 4.4 MB upload in the default high domain) and the GPU builds entity masks. Their only consumer (`avboitVolumeC.glsl` pass 8) reduces to "cell has a proxy interval", which is already known. Provably dead work. | High (CPU) | A1 |
| 2 | Extinction raster (pass 1) runs at 1/8 resolution but with the hardware depth test against the **full-resolution** opaque depth of the private target: cell (x,y) is tested against pixel (x,y). Wrong rejection whenever near opaque geometry covers the top-left 1/8 of the screen. A 64-texel manual loop per fragment then re-tests. | High (bug + GPU) | A2 |
| 3 | `configureDirectRasterShader` sets ~10 uniforms per draw with hashed lookups and calls `gSavedSettings.getS32/getBOOL/getF32` (string map lookups) per draw, in each of the passes. | Med (CPU) | A3 |
| 4 | Emissive draws are submitted in pass 1 where their shader does the 64-texel loop and then returns. Diagnostic atomics (`avboitDiagnostic[4]`, `[5]`) run per fragment in pass 0 regardless of debug mode. | Med | A4 |
| 5 | `RenderAVBOITTileRange` (default on) is fed only by raster pass 0, which in normal mode runs only for GLTF geometry. The feature is inert for ordinary content and, where GLTF exists, rescales a tile inconsistently with its neighbours. | Design issue | A5 |
| 6 | Compaction search: single 256-thread workgroup, shader-storage scan with `memoryBarrierBuffer` per step, 10 candidate dividers over 65536 entries. | Med (GPU, serial) | A6 |
| 7 | Full-resolution color + depth copies into `gAVBOITOpaqueTarget` every frame; color copy only feeds the resolve, which could read/write the screen image in place. | Low-Med | A7 |
| 8 | Dead code: `avboit_cull_fragment()` always false; helper code duplicated three times across `avboitCaptureF/EmissiveF/PbrGlowF`. `materialF.glsl` exits early only for pass 0 (`== 0`) while every other shader exits for `< 2`, so material fragments run full lighting in the extinction pass. | Low | A4, A8 |
| 9 | **Sheer-over-sheer bug** (rear layer a few mm behind a 0.95-alpha garment stays visible). Root cause is representational: both layers share one physical slice and read the same front transmittance. The per-tile ranging that was written to fix it (`RenderAVBOITTileRange`) never runs for ordinary geometry (finding 5), and could not fix a same-slice collision anyway. | High (quality) | A9 |

---

## 2. Phase 0: measure before changing anything

Goal: know which of the Exact OIT costs dominates on the user's machine so the
user can prioritise. Tracy zones already exist (`LL_PROFILE_GPU_ZONE`).

Add one CPU zone name check: `"Exact OIT validation readback"` already wraps
the `glGetBufferSubData` in `FSExactOIT::validateCapture()` (fsexactoit.cpp:1173).
Its duration is the stall. Compare frame time with `RenderOITMode` = Standard
vs Exact OIT in:

1. A scene with almost no transparency (isolates the readback stall + fullscreen
   passes + opaque copy).
2. A heavy sprite scene (particles with glow). Record `EXACT_OIT_PEAK_NODES`,
   `EXACT_OIT_OVERFLOW_COUNT` from the About/diagnostics LLSD
   (`appendDiagnostics`) and watch the log for "node capacity exceeded".
3. An avatar-heavy scene (hair, layered clothing; lists of 2–20).

Also add a one-line periodic log (every 300 frames, behind
`RenderExactOITDebugMode != 0`) printing `control[0]` (nodes used),
`control[3]` (max list), capacity and the number of sort passes issued. This
makes later verification objective.

---

## 3. Exact OIT items

### E1. Overflow: proactive growth and predictive skip (CPU only)

**Goal:** never pay "capture + full vanilla re-render" more than once per
demand increase; never pay it every frame when the cap is hit.

**Why:** `captureOverflowed()` (fsexactoit.cpp:1077) grows the buffer only
after an overflow frame. If demand exceeds `safe_nodes`, capacity cannot grow,
so every frame captures, overflows, discards and re-renders vanilla. Sprites are
the usual trigger (huge overdraw, few nodes reclaimable).

**Change** (all in `fsexactoit.cpp`, own file):

1. Keep per-frame state in `Resources`: `U32 lastRequiredNodes`,
   `U32 skipFramesRemaining`, `U32 consecutiveOverflowsAtCap`.
2. In `validateCapture()` after reading `control`, store
   `sResources.lastRequiredNodes = control[0]`.
3. Proactive growth: at the end of `validateCapture()` when NOT overflowed, if
   `control[0] > capacity * 3 / 4` and `capacity < safe_nodes`, **request**
   growth by recording `pendingGrowthNodes = max(pendingGrowthNodes, control[0])`.
   The actual `glBufferData` runs in `beginFrame()` of the next frame, before
   any capture draw. **Never reallocate the node pool mid-frame**: the
   composite that follows would read the orphaned buffer's fresh,
   uninitialised storage, traverse garbage links and hang the GPU (this
   happened; see `ayanestorm-oit-e1-e4-growth-race.md`). The overflow path
   requests growth the same way. Factor the capacity policy into a pure
   `computeGrownCapacity(required, capacity)` so the skip decision below can
   predict growth without a GL call.
4. Predictive skip: in `captureEligible()` add
   `if (sResources.skipFramesRemaining > 0) { --sResources.skipFramesRemaining; return false; }`
   placed after the `isEnabled()` block. In `captureOverflowed()`, when growth
   is impossible (`grown_capacity <= capacity`), set
   `skipFramesRemaining = llmin(2u << consecutiveOverflowsAtCap, 60u)` and
   `++consecutiveOverflowsAtCap`. Reset `consecutiveOverflowsAtCap = 0` on any
   frame that validates COMPLETE.
   When capture is skipped, `renderPostDeferredCapture()` returns false and
   `lldrawpoolalpha.cpp` runs the vanilla path (already the case), and
   `finishFrame()` returns INACTIVE because `sCaptureCompleted` is false.

**Traps:**
- `captureEligible()` runs for both alpha pools; the skip counter must only be
  decremented once per frame. Decrement in `beginFrame()` instead and only test
  `> 0` in `captureEligible()`.
- Do not skip while `RenderExactOITDebugMode != 0` (the user wants to see the
  diagnostics), just log.

**Verify:** sprite scene that overflows the cap: log shows one overflow then
"skipping N frames", FPS recovers to Standard-mode FPS during the skip. Image is
vanilla (correct) during skip frames, exact otherwise.

**Exactness:** unchanged. Skip frames render vanilla, which is what an overflow
frame renders today.

**Gain:** removes the double render in the pathological case (up to 2× on those
frames). Proactive growth removes one hitch per demand increase.

---

### E2. Glow nodes: drop zero-glow captures, skip glow-less particle draws

**Goal:** stop allocating a second node per particle fragment when glow is 0.

**Why:** `lldrawpoolalpha.cpp:~900` pushes every draw whose vertex buffer has
`TYPE_EMISSIVE` into the emissive list. `llvopartgroup.cpp:637-642` writes the
emissive attribute for every particle unconditionally ("only write glow if it
is not zero" is commented out). So every particle is captured twice; the glow
node has `glow == 0` for most effects and contributes nothing in the composite
(`glow += node.glow`).

**Change A (shader, trivial, exact):**
In `exactOITEmissiveF.glsl` `main()`:
```glsl
void main()
{
    float glow = diffuseLookup(vary_texcoord0.xy).a * vertex_color.a;
    // A zero glow node adds exactly 0 in the composite: allocate nothing.
    if (glow == 0.0) return;
    exact_oit_store_glow(glow);
}
```
Same in `exactOITPbrGlowF.glsl`:
```glsl
    float glow = max(max(emissive.r, emissive.g), emissive.b) * vertex_emissive.a;
    if (glow == 0.0) return;
    exact_oit_store_glow(glow);
```
Vanilla adds `glow` to destination alpha with blend (ONE, ONE); adding 0.0 is a
no-op, so this is exact.

**Change B (CPU, optional, tagged edits):** make `LLDrawInfo::mHasGlow`
accurate for particles so the draw call itself can be skipped.
- `llvopartgroup.cpp` around line 611-642 (`LLVOPartGroup::getGeometry`): the
  function writes `pglow`/`part.mGlow`. Add a tagged block that sets a member
  flag (e.g. `mAnyGlow |= (pglow.mV[3] != 0 || part.mGlow.mV[3] != 0)`) — check
  how `has_glow` is computed at line 849-853 in `LLParticlePartition::getGeometry`
  (`cur_glow.get() != start_glow` = "wrote any glow data"). Replace that with
  "wrote any non-zero glow" behind a tag.
- Then in `FSExactOIT::handleCapturedEmissives()` (own file) filter the vectors:
  ```cpp
  auto drop_no_glow = [](std::vector<LLDrawInfo*>& v) {
      v.erase(std::remove_if(v.begin(), v.end(),
          [](LLDrawInfo* d) { return !d->mHasGlow; }), v.end()); };
  ```
  before calling `pool.renderEmissives(...)`.

**Traps:**
- Change B: `mHasGlow` is also compared in `llvopartgroup.cpp:876` to decide
  whether an existing `LLDrawInfo` can be reused; changing its meaning is fine
  because it is still a per-group boolean. Verify prims (`LLVOVolume`) are
  unaffected: they only have `TYPE_EMISSIVE` when glow > 0, so `mHasGlow`
  stays true for them. Do NOT filter in `lldrawpoolalpha.cpp`; the vanilla path
  must be untouched.
- Change B changes nothing in vanilla mode only if the filter lives inside
  `FSExactOIT::handleCapturedEmissives()`.

**Verify:** Exact OIT debug mode 1 (list depth heat map) over a glow-less
particle emitter: list depth halves. Glowing particles look identical
(compare screenshots against previous build, same frame, camera static).

**Exactness:** exact (A adds nothing; B removes draws that would allocate
nothing after A).

**Gain:** for sprite-heavy scenes roughly halves node writes, atomics and list
lengths from particles. Second-largest sprite win after E7.

---

### E3. Per-draw uniform overhead and dead `oitGlow` uniform

**Goal:** remove three uniform uploads + three hash lookups per alpha draw.

**Why:** `configureCapturedDrawIfActive()` (fsexactoit.cpp:959) runs per draw:
uncached `glUniform1ui` (always re-uploaded), `uniform1f(oitGlow, 0)`
(always 0; only emissive shaders carry glow and they use their own function),
`uniform1i(oitDiscardNoOp)` (constant for the frame). Same in
`configureGLTFCapturedDraw()`.

**Change:**
1. `exactOITCaptureF.glsl`: delete `uniform float oitGlow;`, write
   `oitNodes[index].glow = 0.0;`, and drop `oitGlow == 0.0` from the no-op test.
   Delete `uniform int oitDiscardNoOp;` and make it a permutation:
   in `fsexactoit.cpp` where `addPermutation("EXACT_OIT", "1")` is called for
   each capture program (and in `makeGLTFVariant` via `shader.mDefines` copy),
   add `if (gSavedSettings.getBOOL("RenderExactOITNoOpCapture")) shader.addPermutation("EXACT_OIT_DISCARD_NOOP", "1");`
   and wrap the no-op test in `#ifdef EXACT_OIT_DISCARD_NOOP`. Changing the
   setting then requires a shader reload (the viewer reloads shaders on many
   Render* settings; if `RenderExactOITNoOpCapture` is not in that list,
   document "restart or toggle a graphics preset to apply". It is an A/B debug
   switch, so this is acceptable).
2. Cache the packed blend value per program. In `fsexactoit.cpp` add:
   ```cpp
   namespace {
   struct BlendUniformCache { GLint location = -2; U32 value = 0xffffffffu; };
   std::unordered_map<const LLGLSLShader*, BlendUniformCache> sBlendCache;
   void uploadBlendFactors(LLGLSLShader* shader, U32 packed)
   {
       BlendUniformCache& c = sBlendCache[shader];
       if (c.location == -2)
       {
           static LLStaticHashedString name("oitBlendFactors");
           c.location = shader->getUniformLocation(name);
       }
       if (c.location >= 0 && c.value != packed)
       {
           glUniform1ui(c.location, packed);   // shader is bound by the caller
           c.value = packed;
       }
   }
   }
   ```
   Clear `sBlendCache` in `unloadShaders()` (program objects are recreated).
   Use it in both configure functions; delete the `oitGlow` / `oitDiscardNoOp`
   uploads.

**Traps:**
- Do NOT route the packed blend through `LLGLSLShader::uniform1i`: its value
  cache stores the value as `F32` (llglslshader.cpp:1790), and packed values
  like `0x09010907` exceed the 24-bit exact integer range, so two different
  blend tuples can compare equal and the upload is skipped. That would be a
  silent blend-factor bug.
- GL uniform values are per program object and survive rebinding, so the cache
  keyed by program pointer is valid. It must be cleared whenever programs are
  relinked (`unloadShaders()` + shader reload path).
- `exactOITEmissiveF.glsl` / `exactOITPbrGlowF.glsl` keep their own
  `exact_oit_store_glow`; they never had `oitGlow`.

**Verify:** materials with custom blend (e.g. additive particles
`BF_ONE, BF_ONE`) still composite identically. Debug mode 5 shows blend tuples
per pixel; compare before/after.

**Exactness:** exact.

**Gain:** small CPU per draw; matters in scenes with thousands of alpha draws.

---

### E4. Non-blocking readback: fence + persistent map, overlap with sort pass 1

**Goal:** stop draining the GPU pipeline every frame. The CPU only waits for
the *capture* to finish, while the GPU already works on sort pass 1.

**Why:** `validateCapture()` calls `glGetBufferSubData` which waits for every
command that touches the buffer (in practice, for everything queued) and blocks
the CPU. This removes CPU/GPU overlap for the rest of the frame.

**Design:**
```
capture draws
fence = glFenceSync()
issue sort pass 1 (item E6 shader; safe to run even on an overflowed capture)
glClientWaitSync(fence)               <- waits only until capture is done
read control from persistent mapping  <- no driver copy, no extra sync
if overflow: vanilla fallback (sort results are simply ignored)
else: issue remaining passes (count from control[3]) + composite
```

**CORRECTION (2026-09-03, after the first implementation regressed to ~1 FPS
in sprite scenes):** the first version of this item mapped the control SSBO
itself persistently. That is wrong: a `MAP_READ | PERSISTENT | COHERENT`
buffer is allocated in system memory, and the capture shaders hit the control
buffer with atomics from every fragment, so every atomic crossed PCIe.
The control SSBO must stay device-local; only a separate 16-byte readback
buffer is host-mapped, filled by a GPU-side `glCopyBufferSubData`. Full
diagnosis and code in `ayanestorm-oit-e4-fence-stall-question.md` (Answer
section). The steps below are the corrected version.

**Change (`fsexactoit.cpp`):**
1. In `allocateNodePool()` keep the control buffer exactly as before
   (`glBufferData(..., GL_DYNAMIC_DRAW)`, device-local, never mapped) and add
   a separate readback buffer:
   ```cpp
   sResources.readback = 0;
   sResources.readbackMapped = nullptr;
   if (gGLManager.mGLVersion >= 4.39f && glBufferStorage && glMapBufferRange)
   {
       const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
       glGenBuffers(1, &sResources.readback);
       glBindBuffer(GL_COPY_WRITE_BUFFER, sResources.readback);
       glBufferStorage(GL_COPY_WRITE_BUFFER, 4 * sizeof(U32), nullptr, flags);
       sResources.readbackMapped = static_cast<U32*>(
           glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, 4 * sizeof(U32), flags));
       glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
       if (!sResources.readbackMapped) { glDeleteBuffers(1, &sResources.readback); sResources.readback = 0; }
   }
   ```
   Add `GLuint readback = 0; U32* readbackMapped = nullptr; GLsync captureFence = 0;`
   to `Resources`. Unmap and delete `readback` in `releaseResources()`.
   **Never** create the control SSBO with any `GL_MAP_*` flag.
2. In `renderPostDeferredCapture()` after `markCaptureCompleted()`:
   ```cpp
   if (sResources.readbackMapped)
   {
       glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);   // shader atomics -> copy visibility
       glBindBuffer(GL_COPY_READ_BUFFER, sResources.control);
       glBindBuffer(GL_COPY_WRITE_BUFFER, sResources.readback);
       glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, 4 * sizeof(U32));
       glBindBuffer(GL_COPY_READ_BUFFER, 0);
       glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
   }
   sResources.captureFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
   ```
3. Split `validateCapture()` into two: `beginValidation()` (memory barrier +
   fence exists) and `waitValidation(U32& maximum_list)` doing:
   ```cpp
   if (sResources.captureFence)
   {
       LL_PROFILE_ZONE_NAMED("Exact OIT fence wait");
       GLenum r;
       do { r = glClientWaitSync(sResources.captureFence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull); }
       while (r == GL_TIMEOUT_EXPIRED);
       glDeleteSync(sResources.captureFence); sResources.captureFence = 0;
   }
   U32 control[4];
   if (sResources.readbackMapped) memcpy(control, sResources.readbackMapped, sizeof(control));
   else { glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.control); glGetBufferSubData(...); glBindBuffer(..., 0); }
   ```
4. Restructure `finishFrame()` / `composite()`:
   ```
   if (!captureCompleted etc.) return;                      // INACTIVE checks
   glMemoryBarrier(SHADER_STORAGE | SHADER_IMAGE_ACCESS);
   bindCompositeResources(); set depth off, color mask off
   issue sort pass 1 (always; it early-outs per pixel when count <= K)
   waitValidation(maximum_list)  -> overflow? -> VanillaFallbackScope path (unchanged)
   issue remaining sort passes based on maximum_list (E5/E6 formula)
   composite blend pass, isolate pass 3, debug alpha (unchanged)
   ```

**Traps:**
- The fence must be created *after* the capture draws and *before* pass 1.
- `GL_MAP_COHERENT_BIT` without `GL_MAP_PERSISTENT_BIT` is an error; both are
  required together, plus `GL_MAP_READ_BIT`.
- Any buffer that shaders write with atomics (control, nodes, queues) must
  never be host-mapped: mapped read buffers live in system memory and GPU
  atomics to system memory run at PCIe latency. This was the cause of the
  first E4 attempt's 20x regression in sprite scenes.
- A `static LLCachedControl` read only at allocation time is not a live
  toggle; changing such a setting does nothing until the resource is
  reallocated. Do not use that pattern for A/B diagnostics.
- Never `glBufferData`/reallocate any shader-visible buffer between a
  frame's capture and its composite. Commands issued after the reallocation
  see new uninitialised storage; the list traversals are unbounded and a
  garbage cycle hangs the GPU. All reallocations go through a pending
  request applied in `beginFrame()` (E1, E10b, E8, E9).
- Reading the mapped pointer without waiting on the fence gives stale data.
  Never read it anywhere else.
- Pass 1 runs before the overflow decision. An overflowed capture has `next`
  pointers only to successfully allocated nodes (allocation failure returns
  before linking), so pass 1 cannot read out of bounds. The fallback ignores
  all list data.
- `glDeleteSync` on every path, including fallback and `releaseResources()`.
- Keep the `glGetBufferSubData` fallback for drivers without buffer_storage.

**Verify:** Tracy: "Exact OIT validation readback" disappears; "Exact OIT fence
wait" is short; GPU zone for pass 1 overlaps the wait. FPS in the "almost no
transparency" scene approaches Standard mode.

**Exactness:** exact (same data, same decision).

**Gain:** potentially the largest global FPS gain; removes a full pipeline
drain per frame.

---

### E5. Shallow lists: sort and blend in registers inside the composite pass

**Goal:** pixels with ≤ K nodes (the common case: hair, clothing, glass) need
zero fullscreen sort passes and zero link rewrites.

**Why:** today every pixel with ≥ 2 nodes is relinked by natural merge passes
(reads + writes of `next`) and then traversed again by the blend pass. The
number of fullscreen passes is set by the deepest pixel on screen.

**Design:** counts image semantics become:
- after capture: raw node count `n`;
- after sort pass 1 (only for pixels with `n > K`): `run_count | OIT_SORTED`
  where `OIT_SORTED = 0x80000000u`;
- composite: if `(count & OIT_SORTED) != 0` traverse the sorted linked list as
  today; else load ≤ K `(depth, index)` pairs, insertion-sort in registers,
  apply the opaque cutoff on the sorted array, blend.

CPU issues sort passes only when `maximum_list > K`.

**Change, shader (`exactOITCompositeF.glsl`):**
```glsl
const uint OIT_SORTED = 0x80000000u;
const uint OIT_SHALLOW = 16u;          // K. Try 32 later; measure.
uniform int oitShallowLimit;           // K in normal mode, 0 in any debug mode

// Same total order as comes_first(): greater depth first, lower index first on ties.
bool before(float da, uint ia, float db, uint ib)
{
    return da > db || (da == db && ia < ib);
}

// Blend one node over dst/glow. Extracted verbatim from the existing loop so
// the shallow and deep paths share one implementation.
void blend_node(uint n, inout vec4 dst, inout float glow)
{
    OITNode node = oitNodes[n];
    if (node.blend == 0xffffffffu) { glow += node.glow; return; }
    uint color_src = node.blend & 255u;
    uint color_dst = (node.blend >> 8u) & 255u;
    uint alpha_src = (node.blend >> 16u) & 255u;
    uint alpha_dst = (node.blend >> 24u) & 255u;
    vec4 sf  = blend_factor(color_src, node.color, dst);
    vec4 df  = blend_factor(color_dst, node.color, dst);
    vec4 asf = blend_factor(alpha_src, node.color, dst);
    vec4 adf = blend_factor(alpha_dst, node.color, dst);
    dst.rgb = node.color.rgb * sf.rgb + dst.rgb * df.rgb;
    dst.a   = node.color.a * asf.a + dst.a * adf.a;
    glow    = node.glow + glow * (1.0 - node.color.a);
}

// Shallow path: count <= OIT_SHALLOW, list unsorted.
void blend_shallow(uint head, uint count, inout vec4 dst, inout float glow)
{
    uint  idx[OIT_SHALLOW];
    float dep[OIT_SHALLOW];
    uint n = 0u;
    for (uint node = head; node != OIT_NULL && n < OIT_SHALLOW; node = oitNodes[node].next)
    {
        idx[n] = node; dep[n] = oitNodes[node].depth; ++n;
    }
    // insertion sort, far to near
    for (uint i = 1u; i < n; ++i)
    {
        uint  ki = idx[i]; float kd = dep[i]; uint j = i;
        while (j > 0u && before(kd, ki, dep[j - 1u], idx[j - 1u]))
        {
            idx[j] = idx[j - 1u]; dep[j] = dep[j - 1u]; --j;
        }
        idx[j] = ki; dep[j] = kd;
    }
    // exact opaque cutoff: nearest qualifying node is the last one in far-to-near order
    uint start = 0u;
    if (oitOpaqueCutoff != 0)
        for (uint i = 0u; i < n; ++i) if (is_opaque_cutoff(idx[i])) start = i;
    for (uint i = start; i < n; ++i) blend_node(idx[i], dst, glow);
}
```
Add `uniform int oitOpaqueCutoff;` (the fragment path currently has
`oitFirstSortPass` folded with the cutoff setting; keep that and add this one).

In `main()`, pass 2 (blend), replace the final loop:
```glsl
    uint count = imageLoad(oitListCounts, pixel).r;
    float glow = dst.a;
    if ((count & OIT_SORTED) != 0u)
    {
        for (uint n = head; n != OIT_NULL; n = oitNodes[n].next) blend_node(n, dst, glow);
    }
    else if (count <= max(uint(oitShallowLimit), 1u))
    {
        // Single-node pixels are never sorted by pass 1 (nothing to sort), so
        // they always take this path, even with oitShallowLimit == 0.
        blend_shallow(head, count, dst, glow);
    }
    else
    {
        // Impossible if the CPU issued pass 1 whenever maximum_list > K. Make it visible.
        frag_color = vec4(1.0, 0.0, 1.0, 0.0); return;
    }
    dst.a = max(dst.a, glow);
    frag_color = max(dst, vec4(0.0));
```
Also at the top of pass 2: `if (head == OIT_NULL) { if (oitDebugMode >= 1 && oitDebugMode <= 9) frag_color = vec4(dst.rgb, 0.0); else discard; return; }`
(discard = screen keeps its opaque value; saves a full-screen write).

Pass 1 gating (`oitPass == 1`): replace `remaining_runs > 1u` with
`remaining_runs > max(uint(oitShallowLimit), 1u)` on entry, and OR
`OIT_SORTED` into every count written back by pass 1 and later passes
(`imageStore(oitListCounts, pixel, uvec4(output_runs | OIT_SORTED, ...))`).
Later passes read `remaining_runs & ~OIT_SORTED` for the `> 1` test; pixels
without the flag are skipped (`if ((count & OIT_SORTED) == 0u) return;` in
non-first passes).

Debug modes: the count/depth diagnostics traverse the linked list; they still
work because the list is intact (unsorted) for shallow pixels. Debug mode 4
(order validity) is only meaningful for sorted pixels: skip pixels without
`OIT_SORTED` (paint them dark green) — or set `oitShallowLimit = 0` in all
debug modes, which forces the old behaviour and keeps every diagnostic valid.
Recommended: `oitShallowLimit = (debug_mode == 0) ? K : 0`.

**Change, CPU (`FSExactOIT::composite`):**
```cpp
const U32 K = debug_mode == 0 ? 16u : 0u;   // must equal OIT_SHALLOW in the shader when non-zero
gExactOITCompositeProgram.uniform1i(oit_shallow_limit, (S32)K);
// pass count: see E6 for the chunked formula; without E6:
U32 passes = 0; if (maximum_list > llmax(K, 1u)) { for (U32 w = 1; w < maximum_list; w <<= 1) ++passes; }
```
With E4 the first pass is issued before the fence wait regardless (it
early-outs per pixel); the remaining `passes - 1` after.

**E4 interaction, do not miss:** `issueSpeculativeFirstSortPass()` currently
sets only `oitPass` and `oitFirstSortPass`. Once E5 makes pass 1 read
`oitShallowLimit` (and `oitOpaqueCutoff` if you add that uniform), the
speculative pass must set them too, with the same values `composite()` uses
(`K` when `RenderExactOITDebugMode == 0`, else `0`). Compute `K` once in
`finishFrame()` and pass it to both functions. A speculative pass that runs
with a stale `oitShallowLimit` from a previous frame would sort pixels the
composite then treats as unsorted, or skip pixels it treats as sorted; debug
mode 4 would show red, and mode 0 would show wrong ordering.

**Traps:**
- `count` written by capture is a raw count and can exceed K; only pass 1
  flags. Never OR the flag in capture.
- The `is_opaque_cutoff` semantic must stay "nearest qualifying node and
  everything nearer is kept". In far-to-near sorted order that is "start from
  the last qualifying index". Identical to `prune_behind_opaque_cutoff`.
- Local arrays indexed dynamically compile to local memory; that is fine. Do
  not make K larger than 32 without measuring register pressure.
- Keep `comes_first()` and `before()` bit-identical in semantics (strict `>` on
  depth, `<` on index).
- `blend_node` reads the whole node once, as the current loop does.

**Verify:** debug mode 4 must be all green with K=0. With K=16, compare
screenshots (static camera) of an avatar scene between K=0 and K=16: pixel
identical expected (same order, same arithmetic sequence). Sprite scene: same
comparison.

**Exactness:** exact.

**Gain:** for typical scenes (max list ≤ 16): zero sort passes, no link
writes, one traversal. Biggest GPU win for non-sprite scenes. Depends on:
nothing, but E4's restructure and this item touch the same function; do E4
first.

---

### E6. Deep lists: chunked register sort in pass 1

**Goal:** for lists > K (sprites), cut the pass count from `log2(n)` to
`~log2(n/K)` while keeping the one-pass behaviour for already ordered or
reversed lists (particle groups arrive sorted per group).

**Why:** pass 1 today detects natural runs. A random list of 128 nodes has ~64
runs → 7 passes. With 16-node chunks it has ≤ 8 runs after chunking → 3 passes.

**Change (`exactOITCompositeF.glsl`):** replace `take_natural_run` by
`take_run` and call it from `natural_merge_pass` (all passes; in later passes
runs are already ≥ K so the natural path is taken):
```glsl
const uint OIT_CHUNK = 16u;   // may equal OIT_SHALLOW

uint take_run(inout uint current, out uint tail)
{
    // 1. Measure the natural run starting at `current` (either direction).
    uint head = current;
    uint prev = head;
    uint next = oitNodes[head].next;
    uint length = 1u;
    bool reverse = false;
    if (next != OIT_NULL)
    {
        reverse = comes_first(next, prev);
        while (next != OIT_NULL)
        {
            bool continues = reverse ? comes_first(next, prev) : comes_first(prev, next);
            if (!continues) break;
            prev = next; next = oitNodes[prev].next; ++length;
        }
    }
    if (length >= OIT_CHUNK || next == OIT_NULL)
    {
        // Long (or final) natural run: detach, reverse in place if needed. Same as today.
        current = next;
        oitNodes[prev].next = OIT_NULL;
        tail = prev;
        if (reverse)
        {
            uint p = OIT_NULL, node = head; tail = head;
            while (node != OIT_NULL) { uint f = oitNodes[node].next; oitNodes[node].next = p; p = node; node = f; }
            head = p;
        }
        return head;
    }
    // 2. Short run: gather up to OIT_CHUNK nodes from `head` and sort them in registers.
    uint  idx[OIT_CHUNK]; float dep[OIT_CHUNK]; uint nxt[OIT_CHUNK];
    uint n = 0u; uint node = head;
    while (node != OIT_NULL && n < OIT_CHUNK)
    {
        idx[n] = node; dep[n] = oitNodes[node].depth; nxt[n] = oitNodes[node].next;
        node = nxt[n]; ++n;
    }
    current = node;
    for (uint i = 1u; i < n; ++i)
    {
        uint ki = idx[i]; float kd = dep[i]; uint kn = nxt[i]; uint j = i;
        while (j > 0u && before(kd, ki, dep[j - 1u], idx[j - 1u]))
        { idx[j] = idx[j - 1u]; dep[j] = dep[j - 1u]; nxt[j] = nxt[j - 1u]; --j; }
        idx[j] = ki; dep[j] = kd; nxt[j] = kn;
    }
    for (uint i = 0u; i + 1u < n; ++i)
        if (nxt[i] != idx[i + 1u]) oitNodes[idx[i]].next = idx[i + 1u];   // write only when changed
    if (nxt[n - 1u] != OIT_NULL) oitNodes[idx[n - 1u]].next = OIT_NULL;
    tail = idx[n - 1u];
    return idx[0];
}
```
`before()` is defined in E5. `natural_merge_pass` is otherwise unchanged
(it merges pairs of runs and counts output runs).

**CPU pass count** (in `composite()`), replacing the `width < maximum_list`
loop:
```cpp
U32 passes = 0;
if (maximum_list > llmax(K, 1u))
{
    const U32 runs = (maximum_list + OIT_CHUNK - 1) / OIT_CHUNK;   // upper bound on runs before merging
    passes = 1;
    while ((1u << passes) < runs) ++passes;                         // ceil(log2(runs)), minimum 1
}
```
Rationale: after take_run every run has ≥ OIT_CHUNK nodes except the last, so
`runs ≤ ceil(n / OIT_CHUNK)`; each pass halves the run count.

**Traps:**
- `OIT_CHUNK` in the shader and on the CPU must match; put a comment on both.
- The measurement loop reads the depths once and the gather reads them again
  (≤ 2K reads). Acceptable; do not try to merge the loops.
- The first pass still runs the opaque-cutoff prune before this (unchanged).
- If debug mode 4 ever shows red after this change, the pass-count formula is
  wrong before the shader is: temporarily add one extra pass to confirm.

**Verify:** sprite scene; log the pass count; debug mode 4 all green; image
identical to the previous build with a static camera.

**Exactness:** exact.

**Gain:** deep-list scenes: 2–3× fewer sort passes, and each pass touches only
deep pixels. Depends on E5 (shares `before()`, `OIT_SORTED` gating).

---

### E7. Wave-level node allocation and max-list reduction (subgroup ops)

**Goal:** replace two global atomics per fragment with two per wave.

**Why:** `exactOITCaptureF.glsl:48` (`atomicAdd(oitNodeCount)`) and `:62`
(`atomicMax(oitPad)`) serialize every transparent fragment on the GPU through
one L2 atomic unit. With particle overdraw this is millions of atomics per
frame on one address.

**Change (`exactOITCaptureF.glsl`, and the two glow shaders' copy of the
function):**
```glsl
#ifdef EXACT_OIT_SUBGROUP
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#endif

// Returns 0xffffffffu when no node could be reserved.
// Precondition: helper invocations have already returned (see exact_oit_store),
// so every lane executing this function is a real fragment. That matters:
// atomics issued by helper invocations are dropped by hardware, so a helper
// must never be the lane that performs the wave's atomicAdd.
uint exact_oit_reserve(bool need)
{
#ifdef EXACT_OIT_SUBGROUP
    uvec4 ballot = subgroupBallot(need);
    uint  count  = subgroupBallotBitCount(ballot);
    uint  base   = 0u;
    if (count != 0u)                                   // `count` is wave-uniform
    {
        if (subgroupElect()) base = atomicAdd(oitNodeCount, count);   // lowest active lane
        base = subgroupBroadcastFirst(base);                           // reads that same lane
    }
    if (!need) return 0xffffffffu;
    uint index = base + subgroupBallotExclusiveBitCount(ballot);
#else
    if (!need) return 0xffffffffu;
    uint index = atomicAdd(oitNodeCount, 1u);
#endif
    if (index >= oitNodeCapacity) { atomicOr(oitOverflow, 1u); return 0xffffffffu; }
    return index;
}

void exact_oit_store(vec4 color)
{
#ifdef EXACT_OIT_SUBGROUP
    // All texture sampling (which needs helpers for derivatives) happened
    // before this call, so helpers can leave now. They then count as inactive
    // in every subgroup operation below, and subgroupElect()/BroadcastFirst
    // can only pick a real fragment.
    if (gl_HelperInvocation) return;
#endif
    bool need = true;
#ifdef EXACT_OIT_DISCARD_NOOP
    const uint standard_alpha_blend = 7u | (9u << 8u) | (1u << 16u) | (9u << 24u);
    need = !(oitBlendFactors == standard_alpha_blend && color.a == 0.0);
#endif
    uint index = exact_oit_reserve(need);   // every non-helper lane must reach this call (no early return before it)
    if (index == 0xffffffffu) return;

    oitNodes[index].color = color;
    oitNodes[index].glow  = 0.0;
    oitNodes[index].depth = gl_FragCoord.z;
    oitNodes[index].blend = oitBlendFactors;
    oitNodes[index].next  = imageAtomicExchange(oitHeadPointers, ivec2(gl_FragCoord.xy), index);
    uint pixel_count = imageAtomicAdd(oitListCounts, ivec2(gl_FragCoord.xy), 1u) + 1u;
#ifdef EXACT_OIT_SUBGROUP
    uint wave_max = subgroupMax(pixel_count);       // all lanes still active here? see trap below
    if (subgroupElect()) atomicMax(oitPad, wave_max);
#else
    atomicMax(oitPad, pixel_count);
#endif
}
```
**Trap on `subgroupMax` placement:** after `if (index == 0xffffffffu) return;`
some lanes have exited, so the subgroup op runs on a subset. That is legal
(inactive lanes are excluded, `subgroupElect` picks an active lane) but the
compiler must not hoist it; keep it after the image atomics as shown.

**Trap on `subgroupBroadcast`:** in `GL_KHR_shader_subgroup_ballot` the lane id
of `subgroupBroadcast(value, id)` must be a compile-time constant. Do not use
it with a computed leader. `subgroupElect()` + `subgroupBroadcastFirst()`
(both "lowest active invocation") is the correct pair, and it is only correct
because helper invocations returned before the ballot. Never move the
`gl_HelperInvocation` check below `exact_oit_reserve()`.

**Trap on placement of `#extension`:** these directives must precede every
declaration in the compiled shader string. `exactOITCaptureF.glsl` is linked as
a separate fragment object, so its own top is fine. In
`exactOITEmissiveF.glsl` / `exactOITPbrGlowF.glsl` put them above
`/*[EXTRA_CODE_HERE]*/`. The permutation `#define`s the shader manager
prepends are preprocessor lines and do not conflict.

**CPU (`fsexactoit.cpp`):** decide the permutation once at shader load:
```cpp
static bool subgroupSupported()
{
    // GL_KHR_shader_subgroup + gl_HelperInvocation (GLSL 4.50).
    return gGLManager.mGLSLVersionMajor > 4 ||
           (gGLManager.mGLSLVersionMajor == 4 && gGLManager.mGLSLVersionMinor >= 50)
        ? ExtensionExists("GL_KHR_shader_subgroup", gGLHExts.mSysExts) : false;
}
```
(`ExtensionExists` is a macro in `llgl.cpp`; if it is not visible from
`fsexactoit.cpp`, add a tagged one-line accessor in `llgl.h/.cpp`, e.g.
`bool LLGLManager::hasShaderSubgroup() const` computed in `initExtensions()`,
tagged.) Add `shader.addPermutation("EXACT_OIT_SUBGROUP", "1")` to every
capture program and glow program when supported. Log which path is active.

**Shader version trap:** `llshadermgr.cpp:585-600` emits `#version 430` for
OIT storage shaders. `gl_HelperInvocation` needs 450 (or extension
`GL_ARB_shader_helper_invocation`... it does not exist; it is core 4.50). The
tagged block already special-cases `oit_storage_shader`: extend it to emit
`#version 450` when `minor_version >= 50` (tagged edit, keep the 430 branch).
`#extension` lines must come after `#version` and before any declaration; the
shader manager inserts its own preamble — check where `/*[EXTRA_CODE_HERE]*/`
and the defines land relative to the capture file: the extension directives
must be the first non-comment lines of the *first* file that uses them, i.e.
put them at the very top of `exactOITCaptureF.glsl` and the two glow files,
and make sure the shared shaders that link with the capture object do not
break (they don't use the extension).

**Verify:** debug mode 6 (utilization) unchanged; `EXACT_OIT_PEAK_NODES`
roughly equal before/after in a static scene (helper invocations excluded so
maybe slightly lower). Sprite scene frame time. Test on NVIDIA and AMD if
possible; on a GPU without the extension the log must show the fallback path.

**Exactness:** exact for the image. Tie-break among *exactly equal depths*
uses allocation index; the index order among fragments of different waves is
already arbitrary today, so no determinism is lost. Within a wave, order
becomes lane order (rasterization order), which is closer to vanilla's
submission order than today.

**Gain:** large for sprite overdraw (atomic throughput bound). Also removes the
`oitGlow` uniform from the capture path (E3).

---

### E8. Optional: split nodes into a hot 8-byte sort stream (measure first)

**Goal:** sorting traversals touch 8 bytes per node instead of 32.

**Why:** `comes_first()` reads only `depth`; traversal reads only `next`.
Sorting is memory-latency bound; a smaller stride raises cache-line reuse.

**Change:** two SSBOs: `layout(std430, binding=0) buffer OITLinks { uvec2 oitLinks[]; }`
(`x = floatBitsToUint(depth)`, `y = next`) and
`layout(std430, binding=4) buffer OITPayload { OITPayload oitPayload[]; }` with
`struct OITPayload { vec4 color; float glow; uint blend; uint pad0; uint pad1; }`
(32 bytes; or 24 bytes as `uint data[6]` to save 25% memory; the 32-byte
padded form is simpler). Node capacity, growth, control and all readers
(capture, both glow shaders, composite, compute sort if kept) must switch
together. Depth compare becomes `uintBitsToFloat(oitLinks[i].x)`; do NOT
compare the uint bit patterns (negative zero, NaN semantics) — convert to float
and keep the existing `>` comparison so results stay bit-identical.

**Traps:** two buffers to allocate/grow/free in lockstep; `EXACT_OIT_MEMORY_MB`
diagnostics; the payload struct must stay 16-byte aligned in std430
(`vec4` first).

**Verify:** identical image; measure "Exact OIT natural sort" GPU zone.

**Exactness:** exact.

**Gain:** unknown until measured; likely 10–30% of the sort cost in deep-list
scenes. Skip if E5/E6 already make sorting negligible in the profile.

---

### E9. Compute sort path: remove it (recommended) or make it lazy

**Why:** `RenderExactOITComputeSort` defaults to off. The implementation is
structurally slow: `OIT_MERGE` uses `local_size_x = 1` (one thread per
workgroup), `OIT_BLOCK_SORT` walks the list with lane 0 while 63 lanes wait,
then bitonic-sorts 64 entries with 12 barriers per block. Its two queue buffers
(`4 B * width * height` each, 16 MB at 1080p, 66 MB at 4K) are allocated
whenever the programs compiled, even with the setting off.

**Change (recommended):** delete `exactOITSortC.glsl`, the three programs,
`sortQueues`, `sortQueueCapacity`, `computeSortAvailable`,
`allocateComputeSortQueues`, `sortWithCompute`, `clearSortQueueCount`, the
`used_compute_sort` logging, `oitComputeSortActive` uniform and debug mode 9
in the composite shader, the `RenderExactOITComputeSort` setting (settings.xml
entry and any UI in `panel_preferences_ayanestorm.xml` / `floater_phototools.xml`
— grep for it), and the diagnostics keys `EXACT_OIT_COMPUTE_*`. Update the
how-it-works doc's sort section.

**Alternative (if the user wants to keep it):** allocate the queues only when
the setting is on (check in `allocateComputeSortQueues` and in
`sortWithCompute` on first use) and free them when it is turned off. Do not
attempt to rewrite the compute sorter; E5/E6 give the fragment path the same
work-skipping property.

**Exactness:** exact (fragment path is the default already).

**Gain:** memory (16–66 MB), code size, one less thing to keep in sync with E8.

---

### E10. Memory: initial pool size, shrink policy, opaque copy

**E10a. Initial node pool.** `allocateNodePool()` requests `4 * w * h` nodes.
Change to `2 * w * h` but at least `sResources.peakNodes * 5 / 4` from the
previous allocation (peak survives `releaseResources` unless disabled), so a
session that already needed more keeps it across resizes. Combined with E1's
proactive growth, the first heavy scene grows the pool with at most one
fallback frame.

**E10b. Shrink.** In `validateCapture()` maintain a rolling maximum of
`control[0]` over the last 600 complete frames (`U32 windowPeak`,
`U32 windowFrames`). When the window closes and `windowPeak < capacity / 4`
and `capacity > initial_capacity`, reallocate to
`llmax(initial_capacity, windowPeak * 2)`. Reallocation is a `glBufferData`
(orphan) and, like growth, must be *requested* and executed only in
`beginFrame()` before that frame's capture (same `pending*` pattern as E1).
Never shrink twice within 600 frames.

**E10c. Opaque copy via texture barrier (optional).** Requires
`glTextureBarrier != nullptr` and `gGLManager.mGLVersion >= 4.49f` (or the
`GL_ARB_texture_barrier`/`GL_NV_texture_barrier` extension). The composite
blend pass reads `diffuseRect` only at its own pixel and writes the same pixel
once, which `ARB_texture_barrier` explicitly allows after a
`glTextureBarrier()` call. Change: bind `screen`'s own texture as
`diffuseRect` instead of `sOpaqueTarget`, call `glTextureBarrier()` right
before the blend draw, and drop `copyOpaqueScene()` + `sOpaqueTarget` when
supported (keep the copy path as fallback). The vanilla fallback path is
unaffected (it runs instead of the composite, over the untouched screen).
Isolate pass 3 does not read color. Saves 8 B/px VRAM and a 16 MB copy per
frame at 1080p.

**Traps:** E10c must not be combined with any composite variant that reads
neighbouring pixels (none exists today; keep it that way). If `screen` is
multisampled anywhere (it is not in the deferred path), the barrier rule does
not apply.

**Exactness:** exact.

---

### E11. Small CPU cleanups

- `composite()` line 1365: `gSavedSettings.getBOOL("RenderExactOITComputeSort")`
  per frame → `LLCachedControl` (or delete with E9).
- `shadersReady()` calls `gSavedSettings.getBOOL("GLTFEnabled")` per pool per
  frame and builds a `std::string` → cache the result in a static that is reset
  by `loadShaders()`/`unloadShaders()`.
- `validateCapture()`: `GL_ATOMIC_COUNTER_BARRIER_BIT` is unnecessary (no
  atomic counter buffers are used); harmless, remove for clarity.
- `prepareCaptureBuffers()` binds the FBO to clear two R32UI images; fine.
  Alternative `glClearTexImage` (GL 4.4) avoids the FBO switch; optional.

---

### E12. Fidelity deviations found (user decision)

1. **DoF depth pass skipped.** `lldrawpoolalpha.cpp:242` adds
   `&& !FSOITDispatcher::captureCompleted()` to the vanilla condition that
   renders alpha surfaces with alpha > 0.33 into the depth buffer for
   Depth of Field. With Exact OIT on and DoF on, transparent surfaces do not
   participate in DoF focus. This pass uses vanilla shaders and depth-only
   writes; running it after capture is safe (capture scope has ended, the
   composite does not depend on depth). Recommended: remove the added clause
   (restore vanilla behaviour) — cost is one depth-only alpha pass, only when
   DoF is enabled, exactly as vanilla. The same applies to AVBOIT.
2. **Rigged alpha depth.** Vanilla writes depth for rigged alpha in its first
   pass (a sorting hack). Capture disables depth writes (correct for
   exactness). Consequence: later depth consumers (lens flare occlusion,
   depth snapshots) no longer see rigged alpha. This is arguably more correct
   and must NOT be "fixed" by re-enabling depth writes during capture, which
   would make `early_fragment_tests` reject exact fragments. Document only.

---

## 4. AVBOIT items

AVBOIT is an approximation; some items change the approximation slightly and
are marked. Items marked "identical" do not change any pixel.

### A1. Remove the per-frame Z-bin / RMQ / entity-mask machinery (identical)

**Why:** `rasterizeConservativeBounds()` (fsavboit.cpp:1352-1463) builds
`zbin_min/max` over `avboitVirtualSlices()` bins with a `std::multiset`, then
a sparse table of `slices × levels` words and uploads it every frame
(65536 × 17 × 4 = 4.4 MB with the default high domain). The GPU side
(`avboitBoundsF.glsl`, `avboitVolumeC.glsl` pass 8/9/10) maintains 8-word
entity masks per cell and queries the table. The only consumer is pass 8:
```
if (minimum_bin != 0xffffffffu && merged_mask != 0u) mark 3x3 tile occupancy
```
`merged_mask != 0` holds whenever any entity bit is set in the queried range.
The entity that wrote the cell's interval always set its own bit, and the query
is conservative (an empty RMQ result returns the full ID range `0..0xfffe`),
so `merged_mask != 0` ⟺ `minimum_bin != 0xffffffffu`. The condition is
therefore equivalent to `minimum_bin != 0xffffffffu` alone. The comment in the
shader says the interval is "retained for the future per-entity Z-bin
candidate stage"; that stage does not exist.

**Change:**
- `fsavboit.cpp`: delete lines 1352-1463 (Z-bin build + upload) and the
  `entity_id_uniform` uploads; keep the `bounds` gather, sort, and the proxy
  cube draws (they still feed the proxy intervals). Delete
  `avboitZBinRMQLevels()`, `AVBOIT_ENTITY_MASK_WORDS`, the `AVBOIT_ZBIN_LEVELS`
  permutations, and the corresponding terms in `work_words`
  (`slices * levels` and `cells * 8`). Keep `avboitMaxDivider()`.
- Shaders: remove `avboit_entity_mask_offset`, `avboit_zbin_*`,
  `avboit_mask_for_id_range`, the mask writes in `avboitBoundsF.glsl`
  (`atomicOr(avboitWork[mask_address], ...)`), pass 10's mask write, pass 9's
  mask clear, and in pass 8 replace the `merged_mask` block by
  `if (minimum_bin != 0xffffffffu) { mark 3x3 }`. **Every offset function**
  (`avboit_bounds_offset`, `avboit_proxy_bounds_offset` in the capture, emissive
  and PBR-glow shaders, `avboit_tile_range_offset`, `avboit_dilated_*`,
  `avboit_proxy_miss_offset`) must be updated consistently in all four shader
  files and in `fsavboit.cpp` (`work_words`, `zbin_offset_words`). Grep for
  `AVBOIT_ZBIN_LEVELS` and `* 8u` to find them all.

**Traps:** the work-buffer layout is hand-computed in 5 places; change them in
one commit and bump `shaderCacheRevision()`. Debug mode 6 (proxy coverage) is
unaffected.

**Verify:** debug modes 2, 3, 5, 6, 15 identical before/after; CPU zone for
"AVBOIT occupancy raster" drops by milliseconds in the high domain.

**Gain:** large CPU win every frame; ~5 MB less VRAM and upload.

---

### A2. Pass 1 depth test: use a per-cell max-depth target (bug fix + GPU)

**Why:** `finishDirectOccupancy()` binds `gAVBOITOpaqueTarget` (full-res,
private depth copy) and `beginDirectRasterPass(1)` sets a volume-sized
viewport. `forwardRender` keeps `LLGLDepthTest(GL_TRUE, GL_FALSE)`, so
fragments for cell (x, y) are hardware-tested against opaque depth at
*pixel* (x, y). The manual 64-texel loop `avboit_behind_opaque_bounds()` then
re-tests every fragment against the correct 8×8 block. Wrong hardware
rejections occur whenever near opaque geometry covers the top-left 1/8 of the
screen (a wall, the avatar's shoulder in some camera angles), silently removing
extinction and mis-ordering layers in the affected cells.

**Change:**
1. `allocateVolume()`: allocate `gAVBOITPrepassTarget` with a depth buffer:
   `gAVBOITPrepassTarget.allocate(volumeWidth, volumeHeight, GL_R8, true)`
   (R8 is enough; color is masked off in that pass).
2. New tiny shader `avboitCellDepthF.glsl` (fullscreen triangle, own file):
   ```glsl
   uniform sampler2D avboitOpaqueDepthSampler;
   uniform ivec2 avboitViewport;
   void main()
   {
       ivec2 base = ivec2(gl_FragCoord.xy) * 8;
       float farthest = 0.0;
       for (int y = 0; y < 8; ++y)
       for (int x = 0; x < 8; ++x)
           farthest = max(farthest, texelFetch(avboitOpaqueDepthSampler,
                          min(base + ivec2(x, y), avboitViewport - ivec2(1)), 0).r);
       gl_FragDepth = farthest;
   }
   ```
   Program with `postDeferredNoTCV.glsl` as vertex stage, depth test off,
   depth write on, drawn into `gAVBOITPrepassTarget` once per frame in
   `finishDirectOccupancy()` before `beginDirectRasterPass(1)`.
3. `finishDirectOccupancy()`: bind `gAVBOITPrepassTarget` (not the opaque
   target) for pass 1; `beginDirectRasterPass(1)` viewport stays volume-sized
   (now equal to the target size).
4. `avboitCaptureF.glsl`: delete `avboit_behind_opaque_bounds()` and its call;
   the hardware depth test (LEQUAL against the cell's farthest opaque depth) is
   the same predicate (`gl_FragCoord.z > farthest → reject`). Same deletion in
   `avboitEmissiveF.glsl` and `avboitPbrGlowF.glsl` (their pass-1 branch then
   becomes `if (avboitRasterPass == 1) return;` first thing — see A4).
5. `finishDirectExtinction()` currently calls `gAVBOITOpaqueTarget.flush()`
   then binds it for the early-depth quads; add `gAVBOITPrepassTarget.flush()`
   before that so the target switch is clean.

**Traps:** `early_fragment_tests` is declared in the capture shader; that is
what makes the hardware test cheap. The depth target must be cleared or fully
written every frame (the fullscreen draw writes every cell). Pass 0 in debug
mode 6 still renders into the full-res opaque target: unchanged.

**Verify:** scene with a near wall in the top-left corner and a layered
transparent avatar in the centre: before = ordering errors in the centre,
after = correct. Debug mode 13 (volume vs exact) turns green in those cells.

**Exactness (AVBOIT):** fixes a bug; pixels in affected cells change (for the
better). Unaffected cells identical.

**Gain:** removes 64 texture fetches per pass-1 fragment.

---

### A3. Per-pass uniform setup instead of per-draw (identical)

**Why:** `configureDirectRasterShader()` (fsavboit.cpp:1644) is called per
draw from `configureCapturedDrawIfActive()` and `configureGLTFCapturedDraw()`:
10 hashed lookups, 10 `glProgramUniform*`, three `gSavedSettings` string
lookups (`RenderAVBOITDebugMode`, `wideExtinction()`, `tileRange()`,
`samplingBias()`, `fittedLinearization()`), and `configureAccumulationBlend()`
(3 × 3 GL calls) per draw in pass 2.

**Change:**
- Replace the four settings reads with `static LLCachedControl<...>`.
- In the `render_pass` lambda (fsavboit.cpp:730), before setting
  `sCaptureActive = true`, call `configureDirectRasterShader()` once for every
  AVBOIT program: alpha, PBR alpha, fullbright (+ rigged variants), all
  `gAVBOITMaterialAlphaProgram[i]` with a program object, all
  `gAVBOITGLTFProgram.mGLTFVariants[i]`, the two emissive and two glow
  programs. `glProgramUniform*` does not need the program bound, so this is
  legal without binding. Cache the uniform locations per program in a small
  struct (map keyed by program pointer, cleared in `unloadShaders()`).
- `configureCapturedDrawIfActive()` and `configureGLTFCapturedDraw()` then
  only do: `if (sDirectRasterPass == 2) configureAccumulationBlend();` —
  and even that only once per pass if `LLDrawPoolAlpha` no longer touches
  blend state during capture. Check: `forwardRender` has
  `LLGLDisable oit_capture_blend(GL_BLEND)` while capture is active, which
  disables blending globally (`glDisable(GL_BLEND)` overrides per-attachment
  `glEnablei`). So `configureAccumulationBlend()` must be re-applied after each
  `LLGLDisable`/`LLGLEnable` of GL_BLEND. It is applied per draw today because
  `gGL.blendFunc` is skipped during capture but the `LLGLDisable` scope is per
  `forwardRender` call, i.e. per pass. Re-applying once at the start of each
  `forwardRender(...)` is not possible from our file (upstream function), so
  keep the per-draw `configureAccumulationBlend()` but make it cheap: track a
  `static bool sAccumulationBlendApplied` reset in `beginDirectRasterPass()`
  and by the `LLGLDisable` scope changes... this is fragile. Simplest safe
  option: keep calling `configureAccumulationBlend()` per draw (9 cheap GL
  calls), drop everything else from the per-draw path.
- `oitGlow` uniform: AVBOIT capture uses it in `avboit_direct_store`
  (`alpha > 0.0 || oitGlow > 0.0`, glow accumulation); it is always 0 from
  the color path. Replace with literal 0 in `avboitCaptureF.glsl` (the
  emissive/glow shaders have their own `avboit_store_glow(float)`), delete the
  per-draw `uniform1f(glow, 0)`.

**Verify:** identical image; CPU zone "ra - push batch" per draw shrinks.

---

### A4. Skip wasted emissive work (identical)

- `FSAVBOIT::handleCapturedEmissives()`: `if (sDirectRasterPass == 1) return true;`
  before issuing any emissive draw. Pass 1 emissive fragments contribute
  nothing (the shader returns after the depth loop).
- `avboitEmissiveF.glsl` / `avboitPbrGlowF.glsl`: move
  `if (avboitRasterPass == 1) return;` to the top of `avboit_store_glow` (only
  relevant if the CPU skip above is not done; do both).
- `materialF.glsl:365` (tagged block): the early alpha-only exit tests
  `avboitRasterPass == 0`; every other shared shader tests `< 2`. Pass 1
  therefore runs the full material lighting for legacy-material fragments and
  stores the same alpha (`diffcol.a * vertex_color.a`) at the end. Change the
  condition to match the others (`!= 2` once A9 exists, `< 2` otherwise).
  Identical output.
- `avboitCaptureF.glsl`: `avboit_compare_proxy_coverage()` performs
  `atomicAdd(avboitDiagnostic[4])` per pass-0 fragment and a second atomic on
  miss. Wrap the call in `if (avboitDebugMode == 6)`. Same for the
  `atomicAdd(avboitDiagnostic[2])` in compute pass 5 (one per cell; cheap,
  optional).
- Delete `avboit_cull_fragment()` and its declarations in the shared alpha
  shaders (`#elif defined(AVBOIT) bool avboit_cull_fragment();` in
  `alphaF/fullbrightF/pbralphaF/materialF/pbrmetallicroughnessF`) — these are
  tagged upstream edits; only remove the one declaration line inside the
  existing tag blocks, keep the blocks. Check with grep that nothing else calls
  it.

---

### A5. `RenderAVBOITTileRange`: inert and inconsistent — user decision

**Facts:** `avboit_reduce_tile_range()` runs only in raster pass 0.
`renderPostDeferredCapture()` runs pass 0 for the alpha pools only when
`RenderAVBOITDebugMode == 6`; otherwise pass 0 renders only the GLTF scene.
So with the default settings the per-tile range is empty
(`stored_minimum > stored_maximum`) for every tile without GLTF alpha
geometry and the global curve is used, i.e. the setting does nothing for
ordinary content. Where a tile *does* contain GLTF alpha, the whole tile
(including non-GLTF fragments) is rescaled while its neighbours are not; the
transmittance volume is sampled with linear filtering across cell boundaries,
so neighbouring tiles with different depth mappings blend incompatible slices.

**Options:**
- A. Make it work: call `avboit_reduce_tile_range(bounded_window_depth)` from
  `avboitBoundsF.glsl` for exact proxies (`avboitExactProxy != 0`), which run
  full-resolution over all alpha geometry including rigged. Cost: 2 atomics per
  proxy fragment. Then evaluate the tile-boundary seams; if visible, the
  feature needs a cross-tile blend it does not have today.
- B. Set the default to off (settings.xml) and leave the code.
- C. Remove the feature (shader branches, `avboitWork` tile-range region,
  settings, UI).

Recommendation: B now (zero visual change for non-GLTF scenes, removes the
GLTF-tile inconsistency), and decide A vs C after E-phase profiling. Do not
implement A without the user's approval: it changes AVBOIT's output everywhere.

Context from `ayanestorm-special/doc/...-compute-avboit-implementation-plan.md`
("Next: per-tile depth ranging"): this feature was designed as *the* fix for
the sheer-over-sheer bug, on the stated assumption that "raster pass 0 already
visits every transparent fragment". That assumption stopped being true when
the proxy pass replaced material occupancy (v74), so the fix was never active
in normal mode. Even fully working it cannot separate two layers that share a
slice inside a tile whose depth range also contains anything behind the avatar
(a window, a wall of glass), and it indexes the *global* warp with a
tile-rescaled coordinate. A9 is the direct fix.

---

### A6. Compaction scan: shared memory when the domain fits (identical)

**Why:** compute pass 1 runs one 256-thread workgroup that scans
`AVBOIT_VIRTUAL_SLICES` entries with `memoryBarrierBuffer()+barrier()` after
each step, ten times (candidate dividers), then a Blelloch scan. For 8192
entries this fits in 32 KB of shared memory.

**Change (`avboitVolumeC.glsl`):**
```glsl
#if AVBOIT_VIRTUAL_SLICES <= 8192
shared uint avboitWarpScan[AVBOIT_VIRTUAL_SLICES];
void avboit_scan_barrier() { barrier(); }   // shared memory: barrier() alone suffices
#else
layout(std430, binding = 2) buffer AVBOITWarpScan { uint avboitWarpScan[AVBOIT_VIRTUAL_SLICES]; };
void avboit_scan_barrier() { memoryBarrierBuffer(); barrier(); }
#endif
```
All accesses are already through `avboitWarpScan[...]`; `atomicOr` works on
shared memory. Nothing else changes. For the high domain (default on with ≥ 4
GB VRAM and GL 4.6) this does nothing; note to the user that the high domain
costs a serial single-workgroup scan of 65536 entries × 10 candidates per frame
and that the candidate search could stop early once a fitting candidate is
found (`avboitDiagnostic[1] <= AVBOIT_SLICES` → break; requires a
`barrier()`-uniform break, i.e. read the flag into a local after a barrier and
break uniformly). That early-out is cheap and identical in output: implement it.

**Verify:** debug mode 15 (chosen divider) identical; mode 7 (warp validity)
green.

---

### A7. Drop the per-frame opaque color copy (identical)

**Why:** `beginDirectFrame()` copies screen color and depth into
`gAVBOITOpaqueTarget`. The color copy only feeds `diffuseRect` in the compute
resolve (pass 7), which writes `avboitOutput` = the screen. In a compute
shader, one invocation may `imageLoad` and then `imageStore` the same texel
of the same image without any barrier; no other invocation touches that texel.

**Change:** bind `screen.getTexture()` as `GL_READ_WRITE` at image binding 2,
declare `layout(binding = 2, rgba16f) uniform image2D avboitOutput;` (drop
`writeonly`), replace `texelFetch(diffuseRect, pixel, 0)` with
`imageLoad(avboitOutput, pixel)`, delete the color `glCopyImageSubData` and
the `bindTexture(DEFERRED_DIFFUSE, &gAVBOITOpaqueTarget)` calls. The color
attachment of `gAVBOITOpaqueTarget` is still needed as an FBO attachment for
the raster passes (color masked): allocate it as `GL_R8` instead of
`GL_RGBA16F` (saves 7 B/px). Keep the depth copy (private early-Z target).

**Traps:** debug mode 13 reads the transmittance sampler, not diffuseRect;
unaffected. The isolate pass after resolve reads accumulation textures, not
the copy.

---

### A8. Dead code and duplication (identical)

- Delete `avboit_cull_fragment()` (A4).
- The warp lookup, tile-range and depth helpers are copied verbatim in
  `avboitCaptureF.glsl`, `avboitEmissiveF.glsl`, `avboitPbrGlowF.glsl`. The
  two glow shaders are terminal fragment files (the emissive/glow programs
  replace the fragment stage), so they can link an additional shared fragment
  object the same way `cloneCaptureShader` appends `avboitCaptureF.glsl`:
  factor the shared functions into `avboitCommonF.glsl`, append it to every
  AVBOIT program's `mShaderFiles`, and keep only `main()`/`avboit_store_glow`
  bodies in the terminal files. Optional; reduces drift risk (A1/A2 must edit
  the same offsets three times otherwise).
- `LLGLSLShader::mFeatures.attachNothing = false` on
  `gAVBOITSkinnedBoundsProgram` vs `true` on the non-skinned one: intentional
  (skinning needs the attached skinning object). Leave.

---

### A9. Per-pixel exact front layers: the sheer-over-sheer fix

**Symptom (user report):** wearing a sheer garment a few millimetres over
another sheer layer, the under layer is far too visible. Moving the layers
apart (e.g. between the legs) hides it correctly.

**Established cause** (special-repo plan, sections "Decisive observation",
"v129", "Warp saturation", "v133"): the transmittance volume stores one
summed extinction per physical slice. Two surfaces in the same slice both
read the transmittance *after* the slice, i.e. each includes the other's
extinction. After the v129 self-term removal the rear layer still gets about
24 percent of the colour instead of 5 percent. v133 proved no virtual-domain
size helps: the divider search is driven by the distinct-depth population of
the whole frame, so near layers land in one slice regardless. The summed
integral cannot express order inside a slice. Nothing sampled from the volume
can fix this; only per-pixel knowledge of *which fragment is in front* can.

**Why the previous per-pixel attempt was removed:** V107–V112 implemented a
per-pixel front peel (V107: nearest depth/alpha key bounding the volume
transmittance; V110–V112: peel one, then two layers, store their RGB10 colour
in an SSBO, composite them after the AVBOIT resolve). V112 was reported as
fixing the dress/lace case. V113 removed it because it was an unauthorised
departure from the presentation, not because it failed. The special-repo plan
now records that deviation is authorised. The design below is the same family
but simpler than V110–V112: it stores **no colour**, adds **no custom resolve
step**, and keeps the peeled layers inside the existing normalised weighted
average with exact weights. It is exactly equivalent to source-over for the
front two layers (proof below) and degrades to today's behaviour for deeper
layers.

**Design.** A full-resolution "front key" pass (raster pass 3) records, per
pixel, the two nearest distinct transparent depths with their alpha, using
only 32-bit image atomics. Pass 2 then gives:

- the front fragment F: weight `α_F` (front transmittance exactly 1: by
  definition nothing transparent is in front of it at this pixel);
- the second fragment S: weight `α_S · (1 − α_F)` (exactly the transmittance
  behind F);
- every other fragment x: weight `α_x · min(T_vol_corrected(x), (1 − α_F)(1 − α_S))`
  (today's estimate, but never more than what the two exact front layers allow).

Proof for a pixel with exactly two layers: `Σw = α_F + α_S(1−α_F) = 1 − (1−α_F)(1−α_S) = A`,
the exact aggregate alpha the resolve already uses, so
`colour = (Σ w c / Σ w)·A + opaque·(1−A) = α_F c_F + α_S(1−α_F) c_S + opaque·(1−α_F)(1−α_S)`,
which is source-over. For the reported case (`α_F = α_S = 0.95`) the rear
share becomes 4.8 percent (correct) instead of ~24 percent (v129) or ~50
percent (before v129). With a hair strand in front of the dress, the strand is
F, the dress is S and still exact, and the lace falls to the bounded rear
formula: its weight is at most `(1−α_hair)(1−0.95)`, which already hides it.
That is why two keys are needed (same reason as V112).

**Key format.** `key = (uint(window_depth * 16777215.0 + 0.5) << 8) | uint(alpha * 255.0 + 0.5)`.
24-bit window depth resolves about 6e-8; a 0.1 mm gap at 2 m is about 6e-6,
so depths that differ by 0.1 mm still get distinct keys. The match in pass 2
uses the *quantised depth only* (`key >> 8`), never the alpha bits.

**Two-key atomic insertion** (exact "two smallest distinct values" with
32-bit atomics; duplicates of the front value, e.g. double-sided geometry,
do not consume the second slot):
```glsl
// avboitCaptureF.glsl, pass 3
layout(binding = 0, r32ui) uniform coherent uimage2D avboitFrontKey0;   // nearest
layout(binding = 1, r32ui) uniform coherent uimage2D avboitFrontKey1;   // second nearest, distinct depth
uint avboit_front_key(float alpha)
{
    uint depth_bits = uint(clamp(gl_FragCoord.z, 0.0, 1.0) * 16777215.0 + 0.5);
    return (depth_bits << 8u) | uint(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
}
void avboit_store_front_key(float alpha)
{
    if (alpha <= 0.0) return;                 // invisible texels are not layers
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uint key = avboit_front_key(alpha);
    uint previous = imageAtomicMin(avboitFrontKey0, pixel, key);
    if ((previous >> 8u) == (key >> 8u)) return;          // same depth as current front: duplicate
    uint displaced = key < previous ? previous : key;      // whichever is not the front now
    if (displaced != 0xffffffffu) imageAtomicMin(avboitFrontKey1, pixel, displaced);
}
```
Correctness argument: the final `Key0` is the global minimum m1. Every other
value is either inserted into `Key1` directly (it was not smaller than the
front at its arrival) or was the front and got displaced into `Key1` later.
Values equal in depth to the current front are skipped. Hence
`Key1 = min{ v : depth(v) ≠ depth(m1) }` = m2. Both images are cleared to
`0xffffffff` each frame ("no key").

**Pass 2 weighting** (in `avboit_direct_store`, `avboitRasterPass == 2`,
replacing the `weight` computation; `front_transmittance` is the existing
corrected value):
```glsl
    uint my_depth = uint(clamp(gl_FragCoord.z, 0.0, 1.0) * 16777215.0 + 0.5);
    uint key0 = imageLoad(avboitFrontKey0, pixel).r;
    uint key1 = imageLoad(avboitFrontKey1, pixel).r;
    float alpha0 = key0 == 0xffffffffu ? 0.0 : float(key0 & 255u) / 255.0;
    float alpha1 = key1 == 0xffffffffu ? 0.0 : float(key1 & 255u) / 255.0;
    float front_factor;
    if (avboitFrontLayers != 0 && key0 != 0xffffffffu && my_depth == (key0 >> 8u))
        front_factor = 1.0;                                   // I am the front layer
    else if (avboitFrontLayers != 0 && key1 != 0xffffffffu && my_depth == (key1 >> 8u))
        front_factor = 1.0 - alpha0;                          // I am the second layer
    else if (avboitFrontLayers != 0)
        front_factor = min(front_transmittance, (1.0 - alpha0) * (1.0 - alpha1));
    else
        front_factor = front_transmittance;                   // A/B: old behaviour
    float weight = alpha * front_factor;
```
Use `front_factor` also for the glow term (`max(oitGlow,0) * front_factor`)
and in the two glow shaders' pass-2 branch (glow-only fragments of the front
surface share its depth, so they match `key0` and stay unattenuated, as
vanilla does).

**Why `min()` and not a multiplicative ratio (V108's concern):** `min` never
raises a weight. Where the volume already contains the front layers'
extinction (separated layers), `T_vol` is the smaller term and the result is
unchanged from today. Where it does not (co-sliced, or thin glass missed by
the 1/8 raster), the bound applies. The only distortion is among *rear* layers
when the 1/8 raster missed a thin front pane: rear layers above the bound are
clamped, those below are not, so their ratio shifts by at most the clamp
factor. The front two layers themselves are exact, which is what the eye sees.
This is strictly better than today, where the same rear layers are already
wrong by the missing pane.

**Pass 3 placement and cost.** Insert it in `renderPostDeferredCapture()`
right after `finishDirectOccupancy()` and before the extinction raster:
```cpp
{
    LL_PROFILE_GPU_ZONE("AVBOIT front key raster");
    gAVBOITOpaqueTarget.bindTarget();          // full resolution, private opaque depth
    beginDirectRasterPass(3);                  // viewport full; binds key images 0 and 1
    render_pass(true);
}
```
`beginDirectRasterPass(3)`: clear both key images (`glClearTexImage` with
`0xffffffff`, GL 4.4, or an FBO clear like Exact OIT's head image) and
`glBindImageTexture(0/1, ..., GL_READ_WRITE, GL_R32UI)`. Keep them bound
through pass 2 (the resolve rebinds units 0/1 afterwards; that order is
already how `finishDirectFrame` works). Depth test on, depth write off, colour
mask off (as pass 0). The pass runs the same AVBOIT material programs, so
`gl_FragCoord.z` is bit-identical between pass 3 and pass 2 for the same
fragment (same vertex program, same matrices); the quantised-depth match is
therefore reliable. The shared shaders exit right after computing alpha for
every pass other than 2, so the fragment cost is vertex work + rasterisation +
one texture fetch + two image atomics. `handleCapturedEmissives()` must skip
emissive draws in pass 3 (extend A4's pass-1 skip to `sDirectRasterPass != 2`).
Memory: 8 bytes per pixel (two R32UI images, 16 MB at 1080p).

**Tagged edits in shared shaders** (one token each, inside the existing
`<AS:Chanayane>` blocks): `alphaF.glsl:331`, `fullbrightF.glsl:135`,
`pbralphaF.glsl:203`, `pbrmetallicroughnessF.glsl:~217` change
`avboitRasterPass < 2` to `avboitRasterPass != 2`; `materialF.glsl:365`
changes `== 0` to `!= 2`. The early store then also runs in pass 3 and
`avboit_direct_store` dispatches on the pass value:
```glsl
    if (avboitRasterPass == 3) { avboit_store_front_key(alpha); return; }
```
Put this branch before the existing pass-0 branch. Verify for each shader
that the alpha passed in the early store equals the alpha of the final pass-2
store (they do today for `alphaF`, `fullbrightF`, `pbralphaF`, `materialF`
and GLTF; the key match uses depth, so a small alpha discrepancy only shifts
the bound slightly, never the layer identity).

**Uniform/setting:** `uniform int avboitFrontLayers;` fed from a new
`RenderAVBOITFrontLayers` boolean (default on) so the fix can be A/B compared
live; add it to `configureDirectRasterShader()` (A3: once per pass).
`FSAVBOIT::beginDirectRasterPass()` and `configureDirectRasterShader()` need
no changes for pass 3 beyond what A3 already centralises.

**Diagnostics:** debug mode 16: pixel colour = (has key0, has key1, 0);
mode 17: `alpha0` greyscale. Both read the key images in the resolve compute
(bind them at units 6/7 there, or simply keep 0/1 bound and move the
accumulation images; check the 8-unit limit: the resolve uses 0,1,2,5 and
declares 3,4,6,7).

**Traps:**
- The key images must be cleared *every* frame before pass 3, including the
  first frame after allocation.
- Pass 3 must run before the early-depth tiles are rasterised into the private
  depth (`finishDirectExtinction`), otherwise a conservative tile could reject
  a fragment that is a legitimate second layer. The placement above satisfies
  this.
- `imageAtomicMin` on `r32ui` requires `coherent` and the images bound
  `GL_READ_WRITE`. Do not bind them as `GL_WRITE_ONLY`.
- Never compare alpha bits for identity; compare `key >> 8` only.
- A fragment with `alpha == 0` must not create a key (it is not a layer), and
  correspondingly pass 2 treats it as rear (weight 0 anyway).
- Particles: they are alpha-blended sprites with soft edges; the front key at
  a sprite pixel becomes the nearest sprite. Overlapping additive sprites are
  approximated as source-over by AVBOIT already; the front-two weighting does
  not change that approximation class. Inspect a smoke scene before and after.
- Hair: the front strand per pixel is now exact and unattenuated by strands
  at neighbouring pixels of the same cell. Expect hair to look slightly
  crisper; compare against Exact OIT, which is the reference.

**Verify:** the reported outfit (sheer over sheer, mm apart): mode 0 must
show the under layer at about 5 percent visibility, matching Exact OIT
(`RenderOITMode` = Exact) side by side. Mode 14 (front transmittance) is no
longer meaningful for F/S pixels; use mode 16/17. Then the regression set from
the special-repo plan: avatar behind a window, hair through glass, smoke,
lace edges (mode 13 blue areas), glow objects behind glass.

**Gain:** quality fix for the reported bug with a bounded cost (one cheap
extra traversal, two atomics per fragment, 8 B/px). It does not depend on
tile ranging, the virtual domain size, or the divider search, and it works at
any separation including 0 mm.

---

## 5. Cross-feature safety checklist

Run this list after each phase.

- **Volumetric lighting:** `ASVolumetricLighting::bindTransparencyAtlas()` is
  called by `prepare_alpha_shader` (covers Exact OIT/AVBOIT programs through
  `prepareCaptureShaders`) and again in `lldrawpoolalpha.cpp` at shader switch.
  None of the items above touch program binding order. `materialF.glsl` adds
  `asVolumetricForeground()` into the captured color: untouched.
  `ASVolumetricLighting::renderPass()` runs before `FSOITDispatcher::beginFrame()`
  and its debug mode skips the whole forward pass: untouched.
- **Self-light isolate (pass 3):** E5 changes the composite; pass 3 only reads
  `head != OIT_NULL`, which no item changes. E10c (texture barrier) does not
  affect pass 3 (depth only). Keep the `oitPass == 3` branch first in `main()`.
- **Horizon / procedural sky / lens flares / vignette:** run outside the alpha
  pool and the OIT dispatch; not touched. Lens-flare occlusion reads depth
  after alpha: see E12.2 (unchanged behaviour).
- **HUD, impostor, cube snapshot, mouselook:** `captureEligible()` guards are
  not modified by any item; E1's skip counter must sit after those guards or
  be independent of them (it is per frame, decremented in `beginFrame()`).
- **Standard mode:** every CPU change lives in `fsexactoit.cpp`/`fsavboit.cpp`
  behind `sCaptureActive`/`captureCompleted()`; the tagged edits in E2-B and
  E12 must keep the vanilla code path byte-for-byte when no OIT mode is active.
- **macOS stubs:** `fsexactoit.cpp` and `fsavboit.cpp` have `#if LL_DARWIN`
  stub sections with every public function. Any new public function must get a
  stub there or the Mac build breaks.
- **Shader cache:** bump `shaderCacheRevision()` once per phase that changed
  shader source or SSBO layouts (E3, E5, E6, E7, E8; A1, A2, A6, A7).

---

## 6. Suggested phases and build points

| Phase | Items | Build needed | Shader revision bump |
|-------|-------|--------------|----------------------|
| 0 | Measurement logging | yes (small) | no |
| 1 | E1, E2-A, E3, E11 | yes | yes (E3 changes capture shader) |
| 2 | E4, E5, E6 | yes | yes |
| 3 | E7 (+ `#version 450` tagged edit in `llshadermgr.cpp`) | yes | yes |
| 4 | E9, E10a, E10b, E2-B, E12 (with user approval) | yes | no (unless E9 removes debug mode 9) |
| 5 | E8, E10c (only if profiling justifies) | yes | yes |
| A | A9 (sheer-over-sheer fix), A4 (its emissive skip and `!= 2` hooks) | yes | yes |
| B | A1, A3, A8-dead-code | yes | yes |
| C | A2, A6, A7 | yes | yes |
| D | A5 (decision), A8-dedup | yes | yes |

Phase A is listed first for AVBOIT because it is the user's reported quality
bug; it is independent of the performance items.

Verification protocol for every Exact OIT phase (static camera, same
window size, same time of day):

1. Debug mode 0 screenshot before/after: expected pixel-identical
   (`RenderExactOITDebugMode 0`). Any difference = bug, except E7 in scenes
   with exactly coplanar transparent surfaces (tie-break order is already
   arbitrary there).
2. Debug mode 4: all green (sorted order valid).
3. Debug mode 6: no magenta (overflow) in normal scenes.
4. Debug mode 8 (list depth buckets) unchanged in a static scene.
5. Sprite stress scene: FPS, `EXACT_OIT_PEAK_NODES`, overflow count, pass
   count from the Phase-0 log.

---

## 7. Things deliberately not proposed

- Storing node color as RGBA16F (would halve node payload): the user asked for
  no quality compromise; this would round captured colors to 16-bit before
  blending, which differs from the current fp32 blend chain even though the
  screen target is RGBA16F. Offered only as a future opt-in with a setting.
- Alpha-threshold discard of nearly-invisible fragments (`alpha < 1/255`):
  changes the image; vanilla blends them.
- Rewriting the compute sorter (E9 explains why removal is better).
- Enabling depth writes during capture to restore vanilla's rigged-alpha depth
  (breaks exactness, E12.2).
- Per-tile / bounding-box scissoring of the sort passes: after E5 the passes
  are already cheap for inactive pixels; revisit only if profiling shows the
  fullscreen passes matter at 4K.
