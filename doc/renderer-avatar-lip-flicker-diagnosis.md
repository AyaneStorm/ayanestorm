# Avatar lip-layer flicker: reference comparison and fix review

Date: 2026-11-10  
Author: chanayane@firestorm

## Conclusion and evidence

The latest precision fix is technically justified and can explain why blended lipstick stopped disappearing against cut-out lips. It is not a complete solution to arbitrary coincident avatar layers. Direct source comparison shows that the reference additionally distinguishes baked skin from overlays and gives layered rigged transparency its own ordering/depth policy.

This revision supersedes the earlier inference-only comparison. Both implementations were inspected directly; no renderer code was changed, built, or runtime-tested for this report. The reference's history documents an explicit coplanar worn-layer fix, but still marks its corresponding grid verification as pending. Its implemented algorithm is confirmed; universal runtime success is not.

## What the reference actually does

### Semantic skin/overlay ranks

Before replacing a bake-on-mesh texture reference with its actual image, the reference records whether it names a baked skin slot. Rigged baked-skin faces receive rank zero; other rigged faces receive `1 + (stable object UUID modulo 7)`. Non-skin faces of one object therefore share a positive rank across face boundaries and rebuilds.

Relevant eye-view shaders apply `clip.z *= 1 + rank * 2^-20`. Reversed depth makes positive rank slightly nearer. This was explicitly introduced to prevent recycled draw-record order from exposing alternating bands of body through a coplanar worn layer. It distinguishes skin from overlays independently of opaque/masked/blended classification. Seven positive ranks can collide: this is not a unique ordering for every overlay.

### Camera-relative composition

The reference separates skeletal deformation from the anchor placing its wearer in the region. It subtracts camera position from the placement matrix's translation before composing placement with skinning and multiplying vertices. Projection operates on those camera-relative coordinates. It avoids constructing a large world vertex before projection. Relevant passes share the position calculation and declare invariant clip position.

### Deterministic lists and a separate rigged-alpha algorithm

Opaque/masked GPU lists preserve record order using count, prefix-scan, and scatter rather than unordered atomic append positions. Rigged blended geometry is ordered separately: avatars back to front, attachment points descending, attachment-root local IDs descending, linked parts ascending, then face/index order and stable final ties.

The default rigged-alpha path copies scene depth to a private depth target. For each object in that order:

1. Draw alpha at least 0.5 using premultiplied source-over blending, reversed-depth `GREATER_OR_EQUAL`, and depth writes.
2. Draw softer surviving texels with equal-depth acceptance and no depth writes. Alpha below approximately 0.004 is discarded.
3. After the object's color is complete, write surviving fringe depth to the private target. It then hides later objects without prematurely suppressing the same object's other soft triangles.

Finally, update ordinary scene depth only where rigged alpha reaches 0.5. Faint fringes consequently do not incorrectly occlude later glass, water, or particles. Depth-only updates use strict `GREATER`; the color passes described above accept equality. The private copy addresses layered transparency; it is not by itself the coplanar skin/overlay fix.

## Assessment of the latest fix in our renderer

**Good change:** joint translation is now made relative to the skeleton center before multiplying by the inverse-bind matrix. Previously the translated matrix product rounded at region-coordinate magnitude before its origin was subtracted. This change preserves small differences between layers with different bind transforms.

**Good change:** vertex placement now subtracts the camera from the instance offset before adding local vertex contributions. Previously local detail could disappear when a large world translation was added to each vertex. Float32 at coordinate 128 m cannot retain a 1 micrometre addition. Float4 storage and shader invariance alone did not prevent that intermediate loss.

**Remaining precision caveat:** projection still evaluates a camera-origin term through a world view-projection matrix. That shared term can retain cancellation error, although it does not independently round every layer's local vertex as before. A dedicated translation-free camera-relative projection more closely matches the reference. This remaining difference is not proof that the reported lipstick fix is inadequate.

**Remaining ordering gaps:** our rigged rank is `(alpha-pass category << 3) | min(face index, 7)`, with step `2^-22`. It does not distinguish baked skin from cosmetics. Different objects can tie, and same-pass skin/overlay precedence can depend on face index. The reference uses a skin-zero/object-wide-overlay policy and a four-times-larger per-rank step. Copying its seven-rank hash alone would still leave collisions.

Our OIT raster passes retain strict `GREATER`, so exact depth equality can reject cosmetics against cut-out skin before OIT runs. OIT also derives sorting depth from reciprocal clip-W, ignoring the layer bias applied only to clip-Z, and merges equal quantized depths. This is a separate blended/blended composition issue. It cannot explain a sample already rejected by opaque depth testing.

## What to fix next, and what not to conflate

Keep the precision correction. If the affected lipstick remains stable during animation, close camera movement, large-coordinate placement, and scene-record rebuilds, it is a valid fix for that case. Do not require a transparency rewrite merely to declare that particular precision bug resolved.

For a complete coplanar-layer policy, preserve baked-skin provenance and define stable object-wide overlay precedence instead of relying on alpha-pass and truncated face index. Apply the policy through the same invariant position path and verify genuinely separated surfaces are not reversed by the bias.

For reference-equivalent rigged transparency, implement the ordered/private-depth sequence above and exclude those faces from OIT to avoid double composition. Alternatively retain OIT, but explicitly solve equal-depth admission and effective-depth/layer ordering consistently through its key insertion, transmittance, and tail handling. That would be an adaptation, not the reference's algorithm. Sorting the existing OIT draw list alone does not reproduce ordered source-over blending.

Verification should use constant diagnostic colors with resident textures, the affected real avatars, multiple overlays, and unrelated foreground occluders. Record hardware depth rejection separately from OIT composition. Check alpha near 0.004 and 0.5, forced record rebuilds, and both native backends. These checks distinguish a successful targeted precision fix from unresolved general layer ordering or texture-residency problems.
