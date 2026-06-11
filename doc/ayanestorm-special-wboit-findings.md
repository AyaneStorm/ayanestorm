# WBOIT Transparency Findings

Date: 2026-06-10

## Fresh Conversation Handoff

Current date of this handoff: 2026-06-11.

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

Recommended next manual tests:

- Hair/lashes in front of glass windows.
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

## Open Runtime Issues

- Tinted glasses/windows need retesting after the two-layer WBOIT split. World glass should now composite before avatar/attachment WBOIT, so worn hair/lashes in front of windows should no longer be averaged into the same WBOIT layer as the glass.
- Some glow objects are brighter in WBOIT than vanilla while others match, suggesting a material-specific glow/color interaction still needs isolation.
- Dense rigged hair still needs testing after the skinned-alpha opacity boost. A coarse rigged depth-only prepass made this worse and should not be reintroduced without a better per-material or per-surface rule.
- Alpha-blend textures with nearly opaque pixels and holes, such as cage/fence/vent surfaces, need retesting after the narrow `0.995` coverage-alpha promotion. Global foreground boosts helped these but caused regressions with glass.
- Eyelashes should not be classified as custom blend without direct batch evidence. A user-tested eyelash asset is alpha blend, so if it matches vanilla it may be because its geometry/material path happens to remain visually equivalent, not because it is necessarily outside WBOIT.
