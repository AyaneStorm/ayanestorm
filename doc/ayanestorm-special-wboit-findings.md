# WBOIT Transparency Findings

Date: 2026-06-10

## Fresh Conversation Handoff

Current date of this handoff: 2026-06-12. Updated: 2026-06-13 (legacy-avatar hybrid removed; WBOIT DoF fix retained).

The current WBOIT branch is in an experimental state. It is not just the original single-layer WBOIT implementation anymore. The active direction is the two-layer WBOIT split:

- Layer 1: world/sim alpha is accumulated into WBOIT and composited over the opaque scene.
- Layer 2: the same WBOIT target is cleared/reused, avatar/attachment alpha is accumulated, then composited over the world result.
- Custom blend batches still render in the post-WBOIT legacy fallback after the WBOIT composites.

Important runtime observations from manual testing:

- `RenderWBOITDebugTint` proved that the edited WBOIT fragment shaders are active: enabling it made all transparent WBOIT content magenta/opaque.
- The earlier `HAS_SKIN`-only diagnostic did not affect the tested hair. That means the problematic hair was WBOIT-rendered but not using the `HAS_SKIN` branch, likely an attachment/avatar alpha batch using a non-skinned shader path.
- The removed `RenderWBOITAttachmentAlphaBoost` experiment with the subtle `0.35..0.85` coverage promotion looked slightly better when enabled, but it did not fix hair being too transparent.
- A stronger attachment boost `0.12..0.55` made makeup, hair, and eyelashes too dark and still did not fix the perceived transparency. Do not reintroduce that without a new reason.
- A rigged alpha depth-only prepass was tried and made hair much worse. Do not reintroduce a coarse depth prepass.
- Material WBOIT `mFeatures.hasAlphaMask = true` was tried for parity and regressed hair/eyelashes in front of glass. It remains disabled.
- The near-opaque coverage promotion `0.995..1.0` fixed or greatly improved cage/fence-like cutout alpha textures and should be preserved unless testing proves otherwise.

Latest tested/reverted experiments:

- The previous two-layer WBOIT split tested much better for hair solidity and glass interaction, but made hair, eyelashes, and makeup too dark. A subsequent reveal-softening attempt regressed hair opacity without fixing darkness and was reverted.
- A three-layer split in `pipeline.cpp` / `lldrawpoolalpha.cpp` separated rigged avatar alpha from non-rigged attachment alpha. Manual testing showed a regression: a beard rendered like it was in front of long hair when the hair should visually be in front. The three-layer split and `RenderWBOITThreeLayerSplit` setting were removed.
- Worn eyeglasses now look too opaque, while eyelashes and Lelutka head eyesocket alpha behind the glasses are not toned down by the glass as they were in an earlier renderer state. This is because non-rigged attachments (eyeglasses) and rigged/non-rigged face alpha (eyelashes, eyesocket) are all in the same avatar WBOIT layer and get averaged — WBOIT cannot represent one surface occluding another within the same buffer.

## Layer split + reveal curve revision (2026-06-11, untested)

**Change**: world WBOIT layer now renders both sim-rezzed non-rigged (`ATTACHMENT_NONE`) AND non-rigged attachments (`ATTACHMENT_ONLY`). Avatar WBOIT layer now renders only rigged content (`ATTACHMENT_ALL`, rigged=true).

**Rationale**: eyeglasses are non-rigged attachments. When they were in the avatar layer alongside rigged eyelashes/face alpha, WBOIT averaged them all — glasses appeared too opaque and face alpha showed through them incorrectly. Moving non-rigged attachments to the world layer means glasses composite with the world first, then rigged hair/eyelashes composite on top of the already-attenuated result.

**Risk**: non-rigged worn beard or other non-rigged mesh that visually should be in front of rigged long hair now composites in the world layer (behind the rigged avatar layer). May regress beard-over-hair ordering. Needs testing.

**Reveal curve**: `wboit_reveal_alpha` kept as `pow(1-a, 1.65)` unconditionally. A `HAS_SKIN`-gated split was attempted but reverted — long hair does NOT use `HAS_SKIN` (confirmed in earlier testing), so splitting on that flag would give hair the linear path and make it more transparent. The `wboit_skinned_alpha` boost range was narrowed from `0.35–0.85` to `0.55–0.95`, but this has no effect on hair since hair doesn't use `HAS_SKIN` either. The glass-too-opaque problem remains; the 1.65 exponent is needed for hair opacity and cannot be removed without a hair-specific gate that works for non-skinned hair.

## Avatar Peel Experiment (2026-06-11) — CONCLUDED, REVERTED

`RenderAvatarPeelTransparency` was implemented by Codex as a fixed four-layer depth peel for avatar/attachment alpha, replacing the second WBOIT layer when enabled. The peel shaders (`#ifdef PEEL` blocks in alphaF, pbralphaF, fullbrightF, materialF) and the `avatarPeelCompositeF.glsl` composite remain in the codebase but are unreferenced after the revert. All runtime code (pipeline.h/cpp, lldrawpoolalpha.h/cpp, llviewershadermgr.h/cpp, settings.xml) was reverted to commit `a088dd4f` ("best wboit so far").

### Bugs found during testing

1. **Color mask inherited across peel sub-passes.** `gGL.setColorMask(true, false)` left by the first `forwardRender` disabled alpha writes for all subsequent peel passes. Peel FBO alpha was 0 everywhere; composite discarded everything. Fix: `setColorMask(true, true)` before each peel `forwardRender`.

2. **`is_attachment` not extended for peel mode.** Non-rigged avatar-partition batches were excluded from the `ATTACHMENT_ONLY` pass because the partition-type expansion was guarded by `mForwardToWBOIT` only. Fix: also apply when `mForwardToAvatarPeel`.

3. **Peel layer 0 scene-depth discard gated by `avatarPeelLayer > 0`.** Layer 0 captured all avatar alpha including fragments behind opaque geometry. Composite used `LLGLDepthTest(GL_FALSE)` so peel layers drew over the opaque scene: nose/cheeks disappeared, back-hair bled through body, lace showed through shorts. Fix: moved opaque-depth rejection to the composite shader — sample peel layer depth and `deferredScreen` depth, discard when `peel_depth >= scene_depth + bias`.

4. **Inter-layer epsilon `0.000001` too small.** Coplanar or near-coplanar hair cards and eyelashes collapsed into the same peel layer. Layer 1+ discards treated them as already covered, making them invisible. Raising to `0.0002` helped but exposed the next problem.

5. **Depth precision kills transparency at normal zoom distances.** Even with the scene-depth check moved to the composite, the composite `peel_depth >= scene_depth + bias` comparison failed at typical avatar distances: transparent surfaces on opaque mesh (makeup, lace shorts over panties, lip material) have nearly identical depth values. The correct bias is view-distance-dependent in a non-linear depth buffer. No single constant works across close and far zoom.

6. **Peel layer imprints on sky at far zoom.** At distance, peel depth values spread toward 1.0 (far plane), causing peel fragments to composite onto sky pixels. The sky has `scene_depth = 1.0`; any peel depth < 1.0 passed the composite depth check and painted transparent geometry color on the sky.

### Why the approach was abandoned

Fixed-count depth peeling has fundamental mismatches with SL avatar rendering:

- Dense layered hair typically has 10–30 overlapping alpha surfaces per pixel. Four fixed layers capture only the front few; the rest are silently dropped.
- The inter-layer epsilon must be large enough to separate real layers but small enough not to skip real geometry — impossible to tune for both hair cards (millimeter separation) and body-over-clothing (centimeter separation) at all zoom levels.
- The composite depth test must be loose enough to allow makeup/lace on skin but tight enough to block geometry clearly behind a wall — also not a stable constant in a perspective depth buffer.
- Every iteration re-renders all avatar alpha geometry, making this 4× the cost of a single WBOIT pass for a worse visual result.

**The two-layer WBOIT (world pass + avatar pass) remains the active implementation.** The peel plan document (`doc/ayanestorm-special-wboit-photo-mode-plan.md`) is kept for reference but the approach is not recommended for a follow-up attempt without a fundamentally different strategy (e.g. linearized depth comparison, per-object layer budgets, or a hybrid WBOIT-remainder for overflow layers).

Recommended next manual tests:

- Hair/lashes in front of glass windows.
- Worn eyeglasses over eyelashes and mesh-head eyesocket alpha.
- Hair over opaque pavement/background.
- Hair over head intersections.
- Hair in front of sheer worn clothing, to verify WBOIT still avoids the old vanilla "hair erases sheer shirt" problem.
- Smoke through vents.
- Cage/fence cutout textures.
- Reflective/fullbright objects behind glass.
- Glow brightness parity.

## Summary

The remaining layered transparency artifacts are not only WBOIT algorithm limitations, but several attempted fixes showed that single-layer WBOIT is a poor match for SL avatar hair/lashes when they overlap world glass or opaque backgrounds. The current experiment keeps WBOIT but splits accumulation into world and avatar/attachment layers so worn hair/lashes are not averaged into the same WBOIT solution as windows/glass.

## Findings

1. Avatar and attachment alpha is broadly skipped during WBOIT accumulation.
   - `LLDrawPoolAlpha::renderAlpha()` skips WBOIT when a batch is classified as avatar alpha or has a custom blend function.
   - This excludes normal rigged hair and many normal attachments even when they use standard `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` blending.

2. Self rigged hair is drawn unsorted without depth writes in the post-WBOIT legacy pass.
   - Rigged alpha draw info is not distance-sorted during geometry build; it is batch-sorted.
   - The current post-WBOIT self-rigged pass draws without depth writes, so same-avatar layered hair cards can show through each other and produce visible crossing lines.

3. The GLTF scene-manager alpha render call is unfiltered in non-WBOIT rigged passes.
   - `LLDrawPoolAlpha::forwardRender()` calls `GLTFSceneManager::render(false, ...)` before rigged alpha when not forwarding to WBOIT.
   - In filtered post-WBOIT passes, that global GLTF call does not obey self/other/custom-blend filtering and can create duplicate or unfiltered depth/color side effects.

4. GLTF double-sided bucket rendering can skip valid transparent primitives.
   - `GLTFSceneManager::render(Asset&, variant)` returns when one bucket is empty.
   - It should continue to the next bucket, otherwise primitives in the other bucket are skipped.

5. WBOIT composite lacks invalid-value protection.
   - Dense layered cards can push RGBA16F accum values toward overflow.
   - The composite divides by `accum.a` without finite-value checks, so invalid accumulation can become visible artifacts.

6. Alpha-mask surfaces are not WBOIT targets.
   - Built-in grass/tree alpha-mask paths use hard alpha cutoff and depth writes.
   - WBOIT should target standard smooth alpha blend surfaces, not hard alpha-test surfaces.

## Suggested Rule

Use WBOIT for standard alpha blend surfaces, including avatar rigged hair and normal attachments. Keep legacy fallback narrow:

- Keep alpha-mask / alpha-test faces in their existing hard cutoff paths.
- Accumulate standard `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` alpha-blend faces into WBOIT.
- Use post-composite legacy rendering only for custom blend functions and other cases WBOIT cannot represent directly.

## Fixes Started

- Standard avatar/attachment alpha blend now remains eligible for WBOIT accumulation.
- The post-WBOIT legacy pass now draws only custom blend batches, both rigged and non-rigged.
- The unfiltered GLTF scene-manager alpha call is skipped during the post-WBOIT legacy pass.
- Empty GLTF double-sided buckets now continue instead of aborting the whole render.
- The WBOIT composite now discards invalid accum/reveal samples and clamps reveal before compositing.
- WBOIT-mode glow now renders its emissive sub-pass to the screen alpha channel for bloom instead of being skipped.
- WBOIT color weighting now gives stronger priority to higher-alpha fragments instead of saturating most fragments above alpha 0.1 to nearly equal weight.
- WBOIT shader branches now discard effectively invisible fragments at or below alpha `1/255` before writing accum/reveal, so fully transparent attachments such as collars should not affect hair/glass composition.
- WBOIT color weighting was retuned again to reduce low-alpha foreground glass disappearing into high-alpha background hair/fences: alpha uses a gentler squared curve while depth bias is stronger.
- The near-opaque and skinned/rigged color-weight boosts were reverted after testing showed hair/eyelashes in front of glass made the glass disappear locally. Current active WBOIT weighting keeps the gentler alpha-squared curve with stronger depth bias, without special foreground boosts.
- Material WBOIT shader alpha-mask feature parity was disabled for isolation after it regressed hair/eyelashes in front of glass. The `HAS_ALPHA_MASK` permutation remains active, but `mFeatures.hasAlphaMask` is not enabled for WBOIT material shaders.
- WBOIT shader branches now promote only exact or near-exact opaque output alpha (`0.995..1.0`) toward alpha `1.0` before accumulation/reveal. This targets cutout-like alpha-blend textures such as cages, fences, and vents without treating 1% transparent clothing (`0.99`) as opaque.
- After cage validation, the WBOIT color weight alpha curve was softened from power `2.0` to `1.5`. This keeps reveal/coverage behavior unchanged but gives lower-alpha glass/smoke layers more contribution to the averaged transparent color when dark hair or eyelashes overlap them.
- The WBOIT color weight depth curve was increased from power `6.0` to `8.0`, then to `12.0`, after reflective/background transparent layers remained too visible through the front transparent layer. This is a general front-layer color bias, intended to keep the nearer transparent layer visually consistent without adding material or attachment special cases.
- A WBOIT rigged alpha depth-only prepass was tried and reverted after testing made hair much worse. Real hair assets do not tolerate that coarse depth assist.
- WBOIT reveal/transmittance alpha now uses a separate stronger opacity curve (`1 - pow(1 - a, 1.65)`, clamped toward opaque from `0.95..1.0`) while color accumulation keeps the existing coverage alpha. This targets SL hair cards that were visually too transparent: stacked alpha layers should build opacity faster instead of staying thin.
- Skinned WBOIT shader variants now treat medium/high alpha more like coverage than translucency, promoting alpha toward opaque over `0.35..0.85` before color accumulation and reveal. This is a targeted test for rigged hair/eyelashes whose texture alpha represents strand coverage rather than physical transparency.
- `RenderWBOITDebugTint` was added as a temporary diagnostic. When enabled, all WBOIT fragments render magenta/opaque, proving whether the edited WBOIT shader variants are actually active.
- The `RenderWBOITAttachmentAlphaBoost` opt-in experiment was removed. It applied coverage-style alpha promotion over `0.35..0.85` even when the mesh was not using a `HAS_SKIN` shader variant, but it did not fix the perceived hair transparency; stronger versions made makeup, hair, and eyelashes too dark.
- WBOIT accumulation was first split into two layers: world/sim alpha first, then avatar/attachment alpha over the world result. This substantially improved hair opacity and fixed the "eyelashes in front of glass make the window transparent" problem, but avatar-side content became too dark.
- A global avatar-layer reveal-softening attempt was tested and reverted because it made background visible through hair again while hair, eyelashes, and makeup remained dark.
- A three-layer split was tested and removed: world/sim alpha, rigged avatar alpha, then non-rigged attachment alpha. It regressed attachment beard vs rigged long hair ordering because the fixed rigged-before-attachment composite order could put beards incorrectly in front of hair.

## Session 2026-06-12 — Reveal curve tuning + wboitAvatarLayer uniform

### wboitAvatarLayer uniform (COMMITTED)

Added `uniform int wboitAvatarLayer` to all 4 WBOIT shaders and set it from `LLDrawPoolAlpha::sWBOITAvatarLayer` in `prepare_alpha_shader`. Split `wboit_reveal_alpha`:
- World layer (`wboitAvatarLayer == 0`): linear reveal `mix(a, 1.0, smoothstep(0.95, 1.0, a))` — glass/smoke match vanilla opacity.
- Avatar layer (`wboitAvatarLayer != 0`): boosted reveal with adaptive exponent — hair stacks into solid coverage.

Result: eyeglasses and world transparent objects now match vanilla brightness. Committed.

### Reveal curve tuning (avatar layer)

Tried multiple exponent approaches to fix hair/eyelashes being too dark and having "longer tips" (low-alpha pixels pushed too visible):

| Attempt | Result |
|---|---|
| `pow(1-a, 1.65)` fixed | Hair solid but too dark, eyelashes elongated |
| `pow(1-a, 1.2)` fixed | Hair too transparent |
| `mix(1.65, 1.1, smoothstep(0.3, 0.7, a))` | Better but eyelashes still dark |
| `mix(1.65, 1.0, smoothstep(0.3, 0.7, a))` | Eyelashes still darker than vanilla |
| `mix(1.65, 1.0, smoothstep(0.05, 0.25, a))` | Better tips, hair acceptable, still slightly dark |
| `mix(1.0, 1.65, smoothstep(0.3, 0.7, a))` (inverted) | Currently active — low-alpha tips linear, high-alpha dense regions boosted |

Also added `weight_a = mix(wboit_a, 1.0, smoothstep(0.1, 0.5, wboit_a))` for avatar layer color weight — boosts color contribution without affecting reveal. User confirmed less dark color but hair more transparent. Both active together.

**Key insight**: vanilla avatar alpha is NOT sorted back-to-front by depth. Rigged alpha groups are sorted by `CompareRenderOrder()` (attachment order) per `pipeline.cpp:3896`. Vanilla's correct appearance comes from attachment order happening to work for most hair styles, plus the rigged depth-write pass blocking geometry behind the avatar.

**Key insight**: WBOIT is fundamentally better than vanilla for avatar hair because vanilla's "hair erases shirt" bug comes from batch-level sort (whole hair attachment renders last, overwrites shirt), not per-triangle sort.

### Depth prepass attempt — FAILED, reverted

Tried avatar-layer depth prepass before WBOIT accumulation. Failed immediately: WBOIT requires ALL overlapping fragments to accumulate. Blocking rear fragments with depth test creates holes where the weighted average is computed from incomplete data.

### Legacy avatar with WBOIT world (ABANDONED AND REMOVED)

Several attempts to render the complete avatar stack through the vanilla alpha path after compositing the WBOIT world were tested. Direct rendering and an isolated color/depth target both produced incorrect layered hair, movement trails, or transparency that did not match vanilla.

The experiment has been fully removed, including its setting, render target, composite shader, shader registration, and filtering branches. The renderer is back to the established two-layer WBOIT path. Do not reintroduce a persistent setting for this experiment.

### WBOIT DoF alpha-depth fix (RETAINED)

The deferred alpha depth-only pass used by DoF creates a sharp alpha-card cutout around some unrigged hair. WBOIT now skips that extra transparent depth injection while vanilla continues to use the original pass unchanged. This fixes the reported DoF cutout in full WBOIT without changing full vanilla rendering.

## Session 2026-06-16 — World-reveal attenuation for avatar layer (CONFIRMED PARTIAL)

### wboitWorldRevealFBO blit approach

After world composite, `wboitFBO.attachment[1]` still holds the world-layer reveal data (composite reads it but leaves it intact). A new `wboitWorldRevealFBO` (RGBA16F, no depth) was added to `RenderTargetPack` and allocated alongside `wboitFBO` in `pipeline.cpp`.

After `composite_wboit()` for the world pass, the reveal attachment is blitted to `wboitWorldRevealFBO` via `glBlitFramebuffer` (`glReadBuffer(GL_COLOR_ATTACHMENT1)` selects it as read source). This snapshot happens before `sWBOITClearNeeded = true` resets the FBO for the avatar pass.

In `prepare_alpha_shader`, when `sWBOITAvatarLayer` is true, `wboitWorldRevealFBO` is bound to the new `LLShaderMgr::WBOIT_WORLD_REVEAL` slot (`"worldRevealTex"` sampler). The slot was added to `llshadermgr.h` enum and `llshadermgr.cpp` name array.

All 4 WBOIT fragment shaders (`alphaF.glsl`, `pbralphaF.glsl`, `fullbrightF.glsl`, `materialF.glsl`) got:
- `uniform sampler2D worldRevealTex;` in the `#ifdef WBOIT` block
- `wboit_a *= texelFetch(worldRevealTex, ivec2(gl_FragCoord.xy), 0).r;` when `wboitAvatarLayer != 0`

`texelFetch` used instead of `texture()` to avoid needing `screen_res` (only conditionally declared in pbralphaF.glsl under `#ifdef HAS_SUN_SHADOW`).

**Why the original approach (sampling screen color alpha) was rejected**: `composite_wboit()` uses `gGL.setColorMask(true, false)`, suppressing all alpha writes to the screen buffer. `1 - world_reveal` is computed in the composite shader but never stored anywhere accessible. The blit is the only correct approach.

### Test results (2026-06-16)

- **Eyelashes behind eyeglasses**: CONFIRMED FIXED. Eyelashes are visibly attenuated when behind worn eyeglasses. Stronger glass opacity → stronger attenuation, as expected.
- **Rigged transparent attachments behind glass**: still unattenuated. Rigged hair and a rigged lace vest worn by another avatar both appear unattenuated through a glass panel. Root cause TBD — likely either the avatar layer blit timing, or `prepare_alpha_shader` not binding the texture for those specific shader variants.

### Known tradeoffs

- Screen-space only: hair physically in front of glass at the same screen pixel is slightly over-attenuated. Accepted as minor.
- No world-glass attenuation for non-rigged transparent attachments — those are in the world layer themselves and composite before the snapshot is taken.

## Session 2026-06-17 — Depth-gated world-reveal attenuation (FAILED, REVERTED PARTIALLY)

### Root cause analysis

The hair glitch introduced by commit `128bd6b9fa` ("fixed eyelashes behind glasses") was confirmed by bisection. The `wboit_a *= worldReveal` attenuation suppresses **all** avatar WBOIT fragments at pixels where any world alpha object rendered — not just glass. A tree or fence at alpha=0.5 leaves world reveal=0.5 at its pixels; any avatar hair behind it on screen gets `wboit_a *= 0.5`. When the camera moves, different pixels are evaluated and hair reappears. This is the hair glitch.

### Depth-gated attenuation attempt

Added a third attachment to `wboitFBO` — a `GL_R32F` min-depth buffer (attachment[2]). During world WBOIT accumulation, each fragment writes `gl_FragCoord.z` to attachment[2] with `glBlendEquationi(2, GL_MIN)`, so each pixel stores the depth of the nearest world alpha fragment. After the world composite, attachment[2] is blitted to a new `wboitWorldDepthFBO`. In the avatar WBOIT shaders, the attenuation is depth-gated: `worldReveal` is only applied if `world_depth < gl_FragCoord.z` (world alpha is in front of this avatar fragment).

**Wiring changes**: `WBOIT_WORLD_DEPTH` / `"worldDepthTex"` added to `llshadermgr.h/cpp`. `wboitWorldDepthFBO` added to `pipeline.h/cpp` (allocate, release, blit). `sWBOITClearNeeded` block clears attachment[2] to 1.0 (= far plane = no world alpha) only before the world pass, not the avatar pass. Avatar pass uses only 2 draw buffers so the blit is not overwritten.

**Test results (2026-06-17)**:
- Hair glitch **not fixed**. Transparent hair parts still disappear and reappear with camera movement.
- Eyelash attenuation by eyeglasses **broken** — no longer visible.

### Why it failed

Two separate problems:

**1. Depth comparison is wrong in NDC space.** `gl_FragCoord.z` is the non-linear NDC depth (0=near, 1=far). The min-depth FBO stores the minimum NDC depth across all world alpha fragments. For a world alpha object in front of the avatar, its NDC depth is **smaller** than the avatar fragment's NDC depth — so `world_depth < gl_FragCoord.z` should be true. However if the world alpha object is behind the avatar, `world_depth > gl_FragCoord.z`. In theory this is correct, but if the min-depth buffer was not properly cleared or the blit didn't work, world_depth could be stale values that pass or fail the test incorrectly. The hair glitch persisting suggests the depth gate is either not activating (no attenuation applied at all, breaking eyelashes) or activating everywhere (hair suppressed), depending on what `worldDepthTex` actually contains.

**2. The core problem is that the depth-gate either always fires or never fires**, depending on FBO attachment ordering and whether the blit captures the right data. The `wboitFBO` now has 3 attachments; `addColorAttachment` appends in order, so attachment[2] is the third `addColorAttachment` call (GL_R32F). The blit reads `GL_COLOR_ATTACHMENT2` — this must match the allocation order. If `LLRenderTarget::addColorAttachment` uses a different index scheme internally, the blit reads the wrong attachment.

**3. The eyelash fix breaking** suggests that when `world_depth >= gl_FragCoord.z` (the gate doesn't fire), no attenuation is applied — meaning the world depth in front of eyelashes is reporting as behind them, so the gate never triggers for eyeglasses in front of eyelashes.

## Session 2026-06-18 — Coverage-alpha WBOIT tuning (FAILED, REVERT)

Several shader-side attempts tried to make opaque-face alpha textures behave more like cutout coverage inside WBOIT without touching vanilla rendering:

- Added `wboitCoverageAlpha` in `LLDrawPoolAlpha` only, avoiding `llvovolume.cpp` / `LLDrawInfo` so vanilla geometry batching/rendering would remain untouched.
- First gated coverage on `LLViewerTexture::getIsAlphaMask()`. This did not affect the tested hair. Likely reason: real hair textures contain enough antialias/mip mid-alpha that the existing mask heuristic rejects them.
- Then enabled coverage for alpha textures, including legacy material alpha-blend textures, while excluding PBR alpha-blend and guarding in shader on opaque vertex/face alpha.
- Tried a hard-ish cutout curve `smoothstep(0.25, 0.50, a)`. Result: hair became harsh and alpha-mask-like, with black/jagged cutout edges and lost soft strand tips.
- Tried a softer color coverage curve `1 - pow(1 - a, 2.2)`. Result: hair edges were softer, but the fundamental "hair over sheer dress becomes transparent" problem remained.
- Tried separating color coverage from reveal/transmittance: color `1 - pow(1 - a, 4.0)`, reveal `1 - pow(1 - a, 12.0)`. Result: still visually bad. Screenshot showed broken speckled hairline fragments on the forehead, harsh lashes, over-hardened hair regions, and the underlying hair-over-transparent-dress issue was not solved.

Conclusion: this is not a tuning problem in the WBOIT alpha curves. Treating coverage-alpha hair/knit textures as stronger translucency inside the same WBOIT layer does not reproduce vanilla behavior. These assets are not glass; their texture alpha is coverage for an otherwise opaque surface. When such a surface is stacked over a sheer object, WBOIT averaging/reveal still treats both as transparent participants in one blended solution, so the rear/sheers contaminate the front coverage surface.

Do not continue with blind shader curve tuning for this issue. Revert the `wboitCoverageAlpha` shader/draw-pool experiment.

The next plausible direction should be structural:

- classify alpha-texture/opaque-face surfaces before WBOIT as coverage/cutout surfaces,
- render those with a coverage/alpha-test-like or separate ordered path,
- keep true alpha-blend materials such as glass, smoke, and deliberately sheer fabric in WBOIT,
- avoid reusing shader-side `worldRevealTex` attenuation until the texture-unit/shadow-map conflict is fully understood.

Current known visual failures after rollback target:

- Rigged eyelashes/eyesocket alpha behind worn eyeglasses are still not attenuated like vanilla.
- Rigged hair behind sim-rezzed glass panels is still not attenuated like vanilla.
- Long hair over a sheer dress remains too transparent, especially at farther camera distance; the close/far transition suggests mip/filtering of texture alpha is being interpreted as translucency instead of coverage.

### Conclusion

The depth-gated approach is architecturally correct but the implementation has a depth comparison or FBO attachment ordering issue that was not resolved. The fundamental challenge: NDC depth from `gl_FragCoord.z` is non-linear and monotonically increases with distance, so `world_depth < gl_FragCoord.z` should mean "world object is closer to camera." If eyeglasses are in front of eyelashes, glasses NDC depth < eyelash NDC depth, so the gate should fire. That it doesn't suggests the blit is capturing the wrong data or the min-depth is not being written correctly during the world pass.

### What was not tried

- Verifying what `wboitFBO.attachment[2]` actually contains via debug tint or readback.
- Checking whether `LLRenderTarget::addColorAttachment` correctly maps the third call to `GL_COLOR_ATTACHMENT2`.
- Linear depth for comparison (divide by far plane) to remove NDC non-linearity from the equation.
- An epsilon/bias on the depth comparison to handle coplanar surfaces.

### Current state of the code

The depth-gated implementation remains in the code (not reverted) as of session end. The 4 WBOIT shaders have `frag_data[3]`, `worldDepthTex` uniform, and the depth-gated attenuation block. `wboitWorldDepthFBO` is allocated and blitted. The fix is incomplete and hair glitch + eyelash attenuation are both broken. This needs further investigation before shipping.

## Session 2026-06-17 (continued) — Snapshot isolation fix

### Root cause of hair glitch (RESOLVED)

Two bugs were identified and fixed:

**Bug 1 — Snapshot FBOs cleared during avatar pass.** `wboitWorldRevealFBO` and `wboitWorldDepthFBO` were cleared to 1.0 inside the `sWBOITClearNeeded` block, which fires for BOTH the world pass clear and the avatar pass clear. The blit (world composite → snapshot FBOs) happened between the two clears, so the avatar-pass clear wiped the blit data before avatar shaders could sample it. Fix: moved snapshot FBO clears inside the `if (!sWBOITAvatarLayer)` guard so they only clear before the world pass.

**Bug 2 — Sim-rezzed world objects in reveal snapshot.** The world WBOIT layer accumulates both sim-rezzed objects (`ATTACHMENT_NONE`: fences, windows, trees) and worn non-rigged attachments (`ATTACHMENT_ONLY`: eyeglasses). The original blit captured combined reveal from attachment[1], which included sim fences and windows. Any sim-world alpha object in front of the avatar would depth-gate and attenuate hair. Observed as: ghost window/fence/grill patterns on avatar hair that move opposite to camera rotation (same direction as world objects) — confirmed by user on any avatar, not just those wearing eyeglasses.

**Fix:** Added a 4th attachment to `wboitFBO` (attachment[2] = worn-attachment-only reveal, attachment[3] = min worn-attachment depth). The world WBOIT pass is split into two sub-passes controlled by `sWBOITAttachmentSubPass`:
- Sim sub-pass (`ATTACHMENT_NONE`): writes only attachment[0] (accum) and [1] (combined reveal for composite).
- Attachment sub-pass (`ATTACHMENT_ONLY`): additionally writes attachment[2] (worn-attachment reveal) and [3] (min worn-attachment depth) using same blend modes as [1] and GL_MIN respectively.

The blit after world composite now captures attachment[2] → `wboitWorldRevealFBO` and attachment[3] → `wboitWorldDepthFBO`. Avatar WBOIT shaders sample these — only worn-accessory transparency attenuates avatar hair, never sim-world fences or windows.

All 4 WBOIT shaders updated: `frag_data[4]`, `frag_data[2] = reveal` (attachment-only), `frag_data[3] = depth`. Draw buffer mask controls which writes land in the FBO per sub-pass.

### Eyelash-behind-glasses fix status

Eyelashes behind eyeglasses confirmed fixed by user after Bug 1 fix (snapshot not wiped). Still works correctly after the attachment isolation fix since eyeglasses are `ATTACHMENT_ONLY` — their reveal lands in attachment[2] which is what the avatar shaders sample.

## Session 2026-06-17 (continued 3) — Shadow map ghost pattern root cause (PENDING TEST)

### Root cause of ghost pattern on avatar hair

After the attachment isolation fix landed (session 2026-06-17 continued), avatar hair and eyelashes were reported completely invisible on some avatars. Separately, a "ghost pattern" bug existed: transparent hair and eyelashes were cut by patterns that looked like inverted, rotated, wrong-sized versions of opaque world geometry not visible from the main camera.

The ghost pattern was confirmed to be shadow map data by user observation:
- Ghost objects were different size than the real objects (sometimes smaller, sometimes bigger)
- Same ghost rotated ~90° clockwise when camera was rotated around avatar
- Ghost showed opaque objects that were NOT visible from main camera (e.g. chimneys behind a building)
- Ghost was inverted up/down relative to the world
- Sky appeared transparent; opaque surfaces appeared dark

All of these are characteristic of a shadow map sampled with screen-space coordinates.

### Root cause: texture unit collision via `bindDeferredShader`

`prepare_alpha_shader` in `lldrawpoolalpha.cpp` sets `shader->mCanBindFast = false` and calls `shader->bind()` — but does NOT call `bindDeferredShader`. The deferred shader setup (which binds shadow maps, reflection probes, etc.) is deferred to the first draw batch, which calls `bindDeferredShaderFast`. When `mCanBindFast = false`, `bindDeferredShaderFast` falls through to the slow path and calls `bindDeferredShader`, which in turn calls `bindShadowMaps` and `bindReflectionProbes`.

This means: `bindShadowMaps` runs AFTER `prepare_alpha_shader` set up `worldRevealTex` and `worldDepthTex` bindings. `bindShadowMaps` rebinds shadow maps to their driver-assigned units (in range 0..`mActiveTextureChannels`-1). If `worldRevealTex` or `worldDepthTex` had been assigned units in that same range by `enableTexture`, `bindShadowMaps` would overwrite those bindings with shadow map textures.

When `worldRevealTex` contained shadow map data instead of `wboitWorldRevealFBO`:
- Shadow map values near 0.0 (shadowed) multiplied `wboit_a` toward 0.0 → hair fragments nearly discarded → **all transparent gone** on shadowed avatars
- Shadow map values at non-matching scale/orientation → **ghost pattern** cutting through hair

### Fix: force worldRevealTex and worldDepthTex to units above `mActiveTextureChannels`

All driver-assigned samplers live in units 0..`mActiveTextureChannels`-1. Units at `mActiveTextureChannels` and above are never touched by `bindShadowMaps`, `bindReflectionProbes`, or any per-batch binding code.

The fix replaces `enableTexture`/`bindTexture` with explicit `uniform1i` + `bindManual` using `mActiveTextureChannels` as the forced unit base:

```cpp
// In prepare_alpha_shader, inside if (LLDrawPoolAlpha::sWBOITAvatarLayer):
S32 rev_unit = shader->mActiveTextureChannels;
S32 dep_unit = shader->mActiveTextureChannels + 1;
if (shader->getUniformLocation(LLShaderMgr::WBOIT_WORLD_REVEAL) >= 0)
{
    shader->uniform1i(LLShaderMgr::WBOIT_WORLD_REVEAL, rev_unit);
    gGL.getTexUnit(rev_unit)->bindManual(LLTexUnit::TT_TEXTURE,
        gPipeline.mRT->wboitWorldRevealFBO.getTexture(0));
}
if (shader->getUniformLocation(LLShaderMgr::WBOIT_WORLD_DEPTH) >= 0)
{
    shader->uniform1i(LLShaderMgr::WBOIT_WORLD_DEPTH, dep_unit);
    gGL.getTexUnit(dep_unit)->bindManual(LLTexUnit::TT_TEXTURE,
        gPipeline.mRT->wboitWorldDepthFBO.getTexture(0));
}
```

`uniform1i` bypasses `mapUniformTextureChannel` (which would go through the driver-assigned pool) and writes directly to the GPU uniform. `bindManual` calls `activate()`+`enable()`+`glBindTexture` on the forced unit — safe to call on an otherwise idle unit.

### GL_R32F → GL_RGBA16F

`wboitFBO` attachment[3] and `wboitWorldDepthFBO` were originally allocated as `GL_R32F`. `LLRenderTarget::addColorAttachment(GL_R32F)` calls `setManualImage` with `GL_RGBA` / `GL_UNSIGNED_BYTE` as pixel format/type — mismatched channel count and type for a float single-channel format. Most drivers accept NULL data regardless, but changed to `GL_RGBA16F` for consistency with other WBOIT attachments and to avoid any driver-specific rejection.

### Expected fix outcome (SUPERSEDED BY 2026-06-18 TESTING)

- Hair/eyelashes visible again on all avatars (no more all-transparent-gone from shadow map near-zero values)
- No more ghost shadow-map pattern cutting through avatar hair
- Eyelash attenuation by eyeglasses still works (snapshot data unaffected; only the unit assignment changed)

## Session 2026-06-18 — Worn-reveal attenuation rollback

### User-visible failures confirmed

The attachment-isolated reveal/depth snapshot and texture-unit fix did **not** produce a shippable result.

Observed failures:

- Ghost objects cut through **all avatar-layer transparent content**, not only hair: eyelashes, sheer dresses, knit-hole tops, and other transparent avatar attachments were affected.
- The ghost shapes looked like opaque world geometry sampled from the wrong projection: wrong size, sometimes inverted vertically, sometimes rotated ~90 degrees, and sometimes showing opaque objects not visible from the main camera.
- After forcing `worldRevealTex` / `worldDepthTex` to manual texture units, almost all avatar transparency disappeared. Most avatars lost transparent hair, eyelashes, sheer clothing, and similar alpha content. A few exceptions still rendered, likely because those assets used a different path or technique.
- Removing the depth gate did not restore transparency. This proved `worldDepthTex` was not the only problem.
- Reverting `frag_data[4]` back to `frag_data[3]` and removing the depth snapshot infrastructure still did not fully recover transparency while `worldRevealTex` attenuation remained active.
- Final rollback removed the shader-side `wboit_a *= texelFetch(worldRevealTex, ...).r` attenuation. After that, avatar attachment transparency returned, but eyelashes are again **not attenuated by eyeglasses**.

### Conclusions

The `worldRevealTex` attenuation approach is currently too fragile for the alpha accumulation shaders.

The working theory after testing:

- The shadow-map-looking ghost was caused by `worldRevealTex` effectively sampling data that was not the intended reveal snapshot, or by the reveal binding being overwritten during per-batch deferred shader setup.
- The manual texture-unit fix using `mActiveTextureChannels` was insufficient. Different alpha shader variants can have different active texture-channel counts; a unit that is "free" for one shader can still be touched later by another shader's deferred/shadow binding path in the same frame.
- `frag_data[4]` plus the fourth WBOIT attachment/depth snapshot added extra risk and was not needed once the depth gate was removed.
- The depth-gated approach had already failed earlier; when the texture binding was changed, it exposed worse failure modes rather than fixing the underlying problem.

### Current code direction after rollback

Current intended state:

- Keep the two-layer WBOIT split.
- Keep world/sim alpha and worn non-rigged attachment handling as currently structured unless later cleanup removes unused snapshot plumbing.
- Do **not** use `worldRevealTex` or `worldDepthTex` in avatar WBOIT fragment shaders.
- Do **not** reintroduce `frag_data[4]` / attachment[3] / `wboitWorldDepthFBO` without a new, verified design.
- Accept the known regression for now: eyelashes and mesh-head eyesocket alpha are not attenuated by worn eyeglasses.

Remaining cleanup candidate:

- Some `wboitWorldRevealFBO`, `sWBOITAttachmentSubPass`, and `WBOIT_WORLD_REVEAL` / `WBOIT_WORLD_DEPTH` plumbing may remain as dead or unused infrastructure after the rollback. It should either be removed in a cleanup pass or reused only by a redesigned solution.

### Recommended next approach for eyeglasses

Do not try to attenuate avatar fragments from inside every alpha accumulation shader using a screen-space sampler unless the texture binding path is proven robust across all shader variants and deferred bind paths.

Safer future directions:

- Apply eyeglass attenuation during the avatar WBOIT composite stage, where FBO texture inputs are already explicit and centralized.
- Or add a narrow, separately rendered glasses/face-alpha interaction pass with controlled shader state.
- Or accept the two-layer WBOIT tradeoff and leave eyeglass attenuation unsolved until a cleaner per-object/per-layer ordering design exists.

## Ideas to Try Next — Eyeglasses / Eyesocket Attenuation

The next serious attempt should move the eyeglass attenuation out of the many avatar alpha accumulation shaders and into a single controlled pass.

### Composite-stage worn-accessory reveal mask

Try applying eyeglass attenuation in the avatar WBOIT composite shader instead of in `alphaF`, `pbralphaF`, `fullbrightF`, and `materialF`.

Proposed shape:

- During the world/non-rigged attachment stage, capture a screen-space reveal/transmittance mask for worn non-rigged transparent attachments only (`ATTACHMENT_ONLY`), especially eyeglasses.
- Do not include sim-rezzed world alpha such as windows, fences, trees, vents, smoke, or other scene geometry.
- During the avatar WBOIT composite pass, sample that worn-accessory reveal mask and attenuate the final avatar transparent contribution:

```glsl
avatar_alpha *= worn_attachment_reveal;
avatar_rgb   *= worn_attachment_reveal;
```

This is an approximation, but it avoids binding the mask texture across every alpha material shader variant. The composite path has one shader, explicit FBO inputs, and less interaction with per-batch deferred texture binding.

Expected benefit:

- Eyelashes and mesh-head eyesocket alpha should be dimmed by worn eyeglasses.
- Avatar transparent attachments should not disappear due to shadow/probe sampler contamination.
- Sim-world transparent geometry should not cut patterns into avatar hair/clothing, because it is not part of the worn-accessory mask.

Known limitation:

- Without depth gating, avatar alpha physically in front of glasses may also be attenuated at the same screen pixel. Test whether this is acceptable before adding complexity.

### Add debug visualization before trusting the mask

Add a temporary debug mode that displays the worn-accessory reveal mask directly.

Validation criteria:

- The mask should show worn eyeglasses and similar non-rigged worn transparent accessories.
- The mask must **not** show sim windows, fences, trees, chimneys, shadow-map silhouettes, rotated/inverted geometry, sky, or opaque building shapes.
- If the mask contains anything other than worn transparent accessories, do not wire it into avatar compositing.

### Depth gate only after reveal-only works

Do not reintroduce a depth-gated design first. Prove the reveal-only composite-stage attenuation works and does not break avatar transparency.

If depth is still needed later:

- Capture worn-accessory min depth in a controlled target.
- Sample it only in the avatar composite shader.
- Use a clear debug visualization for the depth mask before enabling it.
- Add bias/epsilon carefully; the old `world_depth < gl_FragCoord.z` approach was not proven stable.

### Alternative narrow pass

If composite-stage attenuation is still too broad, try a dedicated glasses/face-alpha interaction pass:

- Render only worn eyeglasses or explicitly tagged non-rigged transparent face accessories into a small controlled mask.
- Apply that mask only to rigged face-adjacent alpha such as eyelashes and mesh-head eyesocket alpha.
- Avoid making this a general-purpose world alpha solution.

## Open Runtime Issues

- Eyelash attenuation by worn eyeglasses is currently **not fixed** after rolling back shader-side `worldRevealTex` attenuation.
- Verify that avatar transparent attachments remain visible after the rollback: rigged hair, eyelashes, sheer clothing, knit-hole clothing, and other avatar alpha content.
- Verify that the shadow-map/ghost-object pattern is gone after removing shader-side `worldRevealTex` attenuation.
- Glow fix (commit `c1901c36`) confirmed working — glow now matches vanilla.
- Tinted glasses/windows need retesting after the two-layer WBOIT split.
- Dense rigged hair still needs testing after the skinned-alpha opacity boost. A coarse rigged depth-only prepass made this worse and should not be reintroduced without a better per-material or per-surface rule.
- Alpha-blend textures with nearly opaque pixels and holes, such as cage/fence/vent surfaces, need retesting after the narrow `0.995` coverage-alpha promotion.
