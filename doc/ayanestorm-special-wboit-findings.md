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

## Open Runtime Issues

- Tinted glasses/windows need retesting after the two-layer WBOIT split. World glass should now composite before avatar/attachment WBOIT, so worn hair/lashes in front of windows should no longer be averaged into the same WBOIT layer as the glass.
- Worn eyeglasses over face alpha are not matching earlier renderer behavior: glasses can look too opaque while eyelashes/eyesocket alpha behind them are not attenuated by the glasses.
- Some glow objects are brighter in WBOIT than vanilla while others match, suggesting a material-specific glow/color interaction still needs isolation.
- Dense rigged hair still needs testing after the skinned-alpha opacity boost. A coarse rigged depth-only prepass made this worse and should not be reintroduced without a better per-material or per-surface rule.
- Alpha-blend textures with nearly opaque pixels and holes, such as cage/fence/vent surfaces, need retesting after the narrow `0.995` coverage-alpha promotion. Global foreground boosts helped these but caused regressions with glass.
- Eyelashes should not be classified as custom blend without direct batch evidence. A user-tested eyelash asset is alpha blend, so if it matches vanilla it may be because its geometry/material path happens to remain visually equivalent, not because it is necessarily outside WBOIT.
