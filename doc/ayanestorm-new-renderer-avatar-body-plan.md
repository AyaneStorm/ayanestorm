# New renderer, step e: system avatar body

## Context
The native renderer draws rigged mesh and attachments, but not the system avatar body: head, upper and lower body, eyes, eyelashes, hair, skirt. Avatars without a mesh body, and parts a mesh body does not cover, are missing. The renderer library already has everything needed and is tested: GPU morph sets (`addMorphSet`, `addMorph`, `setMorphWeights`, `setMorphMask`, `morphVertex()` in `asrSceneSkinC.glsl`), skins and skinned or attached instances. Only the viewer side is missing. There are no library or shader changes, so Vulkan and Metal stay as they are.

## How the viewer draws the body (facts this plan copies)
- **Meshes:** `LLVOAvatar::mMeshLOD[MESH_ID_*]`, each with LOD children `LLAvatarJointMesh` (`mMeshParts`). A LOD mesh indexes the reference mesh's vertices (`LLPolyMeshSharedData::mReferenceData`; LOD vertex count = largest index + 1). The library's LOD model is the same (shared vertices, coarser indices, as the test creature does).
- **Skinning** (`LLViewerJointMesh::uploadJointMatrices()`, `avatarSkinV.glsl`): palette entry i = `mJointRenderData[i]` world matrix × translation by its pivot (`mRootToJointSkinOffset`, or the next entry's `mRootToParentJointSkinOffset` for a parent entry with no skin joint), at most 15 entries. A vertex weight w gives joint `floor(w)` with share `1 - fract(w)` and joint `min(floor(w)+1, 14)` with `fract(w)`. Normals use the same matrix.
- **Unweighted meshes** (eyeballs) are drawn rigidly with the joint mesh's own world matrix.
- **Morphs** (`LLPolyMorphTarget::apply()`): sums of deltas × `getLastWeight()` × the vertex mask, with normal and binormal deltas × 0.65. A degenerate binormal delta becomes (1,0,0). `getLastWeight()` is exactly what the CPU mesh holds, sex and pending masks included.
- **Rules** (`renderSkinned()`, `renderTransparent()`, `renderRigid()`):
  - head, upper, lower: masked at 0.2 (`LLDrawPoolAvatar::sMinimumAlpha`), each only if `isTextureVisible(TEX_*_BAKED)`.
  - skirt (only when worn), eyelashes, hair: blended. Hair and skirt are `isTransparent()`: no culling.
  - eyes: rigid, masked at 0.2.
  - self: nothing if `!gAgent.needsRenderAvatar()`; head, eyelashes, hair and eyes hidden if `!gAgent.needsRenderHead()` (mouselook).
  - `updateMeshVisibility()` hides a part when an attachment uses its bake (`mVisible`).
  - Jellydoll draws head, upper, lower and eyes, no hair. Invisible draws nothing.
- **Texture:** `mTexture` (server bake), or the layer set composite (self, local bake), or `IMG_DEFAULT`; colour `mColor`.
- **LOD:** the first valid child after `LLVOAvatar::updateJointLODs()` (public), from the pixel area. Native mode no longer runs it, so we call it.

## Design
New module **`indra/newview/asravatarbody.cpp/.h`** (`ASRAvatarBody`), so `asrworld.cpp` (3,574 lines) does not grow.

### Shared, once per reference mesh (`LLPolyMeshSharedData*`)
- **Morph set:** base coords, normals, binormals and UVs, plus every `LLPolyMorphData` of the mesh as a target. A target is masked if the first avatar's param has `mVertMask` or pending masks (these come from `avatar_lad`, so they are the same for every avatar). Also a target-index map.
- **Skin:** one `asr::SkinDesc` from the `mJointRenderData` walk: pivot translations, and skeleton joint numbers (`getJointNum()`). A parent entry's joint is found by walking up from the next entry's joint to the matching world-matrix pointer.
- **Geometry per LOD:** `asr::buildGeometry` over the reference vertices `0..N-1` with that LOD's indices and the skin weights above. One shared box per mesh. No coarser chain: the CPU picks the LOD, as the viewer does.
- **Eyeballs:** geometry with no weights.

Shared data stays for the session, like `sSkins`. It is a few MB at most.

### Per avatar (`Body`)
- One `addMorph` per skinned reference mesh.
- Its instances: `skeleton`, `skin`, `morph`, the chosen LOD's geometry, a material.
- Eyeballs: an attached instance (`joint` = nearest skeleton ancestor, `objectToWorld` = mesh world × inverse of that joint's world, recomputed when the avatar changes).
- A cached list of `(LLPolyMorphTarget*, morph, target)` for weight polling.

### Per frame (from `updateAvatars()`)
For every non-dead, built avatar that is not animesh or a UI avatar:
1. Poll the cached last weights. Send `setMorphWeights` only when one changed (physics jiggle included).
2. Poll per part: chosen LOD (`updateJointLODs()` when the pixel area changed by over 10% or 1 s passed), visibility (the rules above), texture pointer, colour, appearance mode. Rewrite the instance or material only on change.
3. Masks: send `setMorphMask` only when flagged (see the hook below).
4. Every so often, visit the body's textures for decode priority, as `visitTextures()` does.

### Skeleton ownership
- `AvatarSkeleton` gains a `Body` (`std::unique_ptr`) in `asrworld.cpp`. Every avatar with a drawn body gets a skeleton, not only avatars with rigged objects.
- The skeleton stays while `users` is not empty or the body exists. Avatar death releases the body (instances, morphs, materials) before `removeSkeleton`.

### Access to `asrworld.cpp` internals
A small internal header `asrworldshared.h`, with thin wrappers defined outside the anonymous namespace:
- `scene()`
- `useMaterial()`, `releaseMaterial()`, `materialId()`
- `visitMaterialTexture()`
- `rowsOf()`

The body module uses the avatar's `mDrawable` as the texture/material user key.

### LL/FS file edits (minimal, tagged `<AS:Chanayane>`)
- `friend class ASRAvatarBody;` in:
  - `LLPolyMeshSharedData` and `LLPolyMorphTarget` (`llpolymesh.h`, `llpolymorph.h`)
  - `LLAvatarJoint` and `LLAvatarJointMesh` (`llavatarjoint.h`, `llavatarjointmesh.h`)
- One hook in `LLVOAvatar::applyMorphMask()` (`llvoavatar.cpp`): `ASRAvatarBody::masksChanged(this)`. Masks are regenerated in place, so polling cannot see them cheaply.
- Add the new files to `asr_sources.cmake`.

### Diagnostics
`logCounts()` gets bodies, drawn parts, morph weight updates and mask updates. The plan of the log stays temporary like the others.

## Open after this step (recorded in the handoff)
- Local bake composites (self while editing appearance) are GL render targets. Whether they reach a renderer slot is unverified; if not, the default image shows.
- Blended parts (hair, eyelashes, skirt) have no 0.2 discard in Mac OIT, unlike the viewer's post-deferred pass.
- Jellydoll body colour uses `getMutedAVColor()`. Hiding attachments for jellydoll and invisible avatars is the attachment path's job, so it is a separate item.
- Cloth wind (`mClothingWeights`, `RenderAvatarCloth`) is not drawn.
- P4 shadows: the self head hidden in mouselook must still cast shadows.

## Verification
- glslang: no shader changes.
- The standalone test does not compile viewer files. One user viewer build (it also carries the unbuilt LOD-cap fix). Check:
  1. An avatar with a system body: shape sliders and physics move the body (morph weights).
  2. Alpha layers hide parts: a mask, then a full alpha with a mesh body (`mVisible` plus the bake's alpha at 0.2).
  3. Skirt, hair, eyelashes, eyes. Eyes follow eye motion.
  4. Mouselook hides your own head; the body stays.
  5. Walking away switches LODs without holes.
  6. A jellydolled avatar shows a flat colour.
  7. The log shows bodies and parts, with no budget or validation errors.
- Mac: the next Metal batch, no Metal-specific code.
- After approval: copy this plan to `doc/ayanestorm-new-renderer-avatar-body-plan.md` with `cp`.
