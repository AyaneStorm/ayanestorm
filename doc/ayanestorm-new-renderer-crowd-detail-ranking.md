# Crowd detail ranking: nearest avatars at full detail

Target: events of up to 150 avatars, on cards with 4 to 12 GB, without spilling into
shared system memory.

## As implemented (2026-11-11, unbuilt)

- `asr::AvatarDetailPolicy` (`asravatarresidency.h`): full-detail count follows the
  non-texture share of the working budget (lower above 45%, raise below 35%, one step
  per 4 s, floor 8). Near avatars are protected instead (10 m, gentle steps): at 55%/45%
  all 82 avatars of a club stayed full, skinning and drawing took 30 ms of GPU time, and
  the frame rate fell to 20. Past the full-detail count, levels 2 then 1 in bands of
  max(full/2, 4) ranks. Mesh also gives way, whatever its share, when the total is past the
  budget while textures are at their floor (near range 4 m), and detail returns only below
  90% of the budget: at 86 avatars a 50% share kept everyone full, the total passed the
  budget, the driver spilled and the UI vertex buffer was refused.
- Per-frame buffers (`growBuffer()`, the Metal UI buffer) are counted anyway, never refused.
- A level is a factor on the viewer's LOD factor (0.75, then 0.5), not a hard LOD cap: a
  hard LOD 1 showed creators' broken low LODs as shards at a few metres.
- `setAnimationPeriods()` ranks in view first, then nearest. Always full: own avatar,
  camera focus, always-render avatars, and any avatar in view within 10 m.
  `LLVOVolume::calcLOD()` scales its LOD factor by `ASRWorld::avatarLodScale()` for
  attachments and animesh, so fetch, residency, skinning and rewrites follow unchanged.
- Orphaned attachments (`LLViewerObjectList::orphanize()`) leave the renderer, as OpenGL
  hides them; drawn, they stood frozen in their last pose after their avatar left.
- Textures: `nearestUse()` gives each texture the distance of its nearest user (0 for
  always-full). The near range (`sNearMetres`) is 20 m and narrows by 20% a second while
  textures exceed their limit, down to 4 m, widening by 10% a second below 90% of it. Under
  bias, sizes past the range shrink with (distance / range) squared; near ones are not
  shrunk. Near, in-view textures are never refused an update while the total is under the
  working budget; within 4 m never at all. A fixed 20 m failed at 88 avatars: nearly all
  textures were near, the total reached the budget, and nothing could refine.
- First images never wait at the limit: they enter at 64 px and refine
  (`LLViewerFetchedTexture::createTexture()`). Waiting left worn mesh gray at 1 m.

## Measured cost (club session, 2026-11-11, RTX 3080 Ti)

At 60 to 65 avatars after the 85% texture steering change:

- Working budget 7.0 to 7.5 GB; tracked total 6.2 to 6.4 GB; driver 8.1 of 10.1 GB.
- Geometry chunks 2240 MB, tables 640 to 676 MB (deformed positions 337 to 372 MB,
  clusters 240 MB).
- Textures squeezed to a 2.3 to 2.7 GB limit and held over it, which freezes refinement:
  close avatars sharpen slowly even with their textures in the disk cache.

Roughly 100 MB per avatar. 150 avatars would need about 15 GB.

## Rule

Avatars are ranked by distance to the camera. The nearest get full mesh and texture
detail. Past what the budget allows at full detail, each further rank step lowers
the allowed detail by one level, down to the coarsest. Every avatar stays visible.
No avatar is held back for detail (see the avatar display rule: show first, refine).

## Mechanism

1. **Per-avatar detail level.** `setAnimationPeriods()` already ranks skeletons each
   update. It also assigns each avatar a level 0 (full) to 3 (coarsest) from its rank and
   a budget share. The level is stored on `AvatarSkeleton`.
2. **Budget share from memory, not a fixed count.** The number of full-detail avatars
   comes from the free working budget divided by the measured average cost of a
   full-detail avatar, so a 12 GB card keeps many at full detail and a 4 GB card keeps a
   few. Hysteresis on rank changes keeps the levels from flickering as people move.
3. **Mesh.** The avatar's level caps the finest LOD its worn shapes may keep resident
   (`asrmeshlodresidency.h`) and may select on the GPU (a per-skeleton finest-LOD clamp
   next to the existing `lodScale`). A capped avatar frees finer LOD geometry and, through
   smaller skinned vertex counts, deformed-position table space.
4. **Textures.** The level caps the finest mip its textures may request, through the
   existing per-texture `detailLimit` path, so far avatars stop competing with near ones.
5. **Return of detail.** When the camera moves, the newly nearest avatars are raised
   first and the farthest are lowered first, inside the same per-frame upload limits.

## Interaction with existing policy

- Off-screen parking (`asravatarresidency.h`) stays: it releases skinning data for
  avatars out of view; ranking applies to avatars in view.
- The 85% texture steering stays; ranking lowers demand so textures fit under it.
- Allocator block slack (~1.3 GB) and geometry chunk holes (~0.6 GB) are separate
  items, measured after ranking lands.

## Always full detail (decided by the user, 2026-11-11)

These are level 0 regardless of rank, and are counted first against the budget share:

- The local avatar.
- Avatars set to always render fully: `LLVOAvatar::getVisualMuteSettings() == AV_ALWAYS_RENDER`.
- Whatever the camera focuses on, avatar or object: `gAgentCamera.getFocusObject()`. For a
  worn attachment, its wearer; for an object, its linkset and its textures.

## Textures: sharp near, soft far (all surfaces)

Goal (user, 2026-11-11): anything in view and not too far is sharp; farther things do not
need sharp textures. This applies to world objects as well as avatars.

Today, at the texture steering limit every refinement is frozen alike
(`sFreezeImageUpdates`), with one exemption for textures at 512 texels or more of GPU
demand (`canRefineVisibleTexture()`). A nearby wall needing 256 texels stays blurred.

Replacement: the freeze becomes ordered by distance instead of uniform.

1. Each tracked texture keeps the distance of its nearest in-view user (object or
   wearer), from the same pass that sizes it.
2. A texture within the near range refines to its GPU-feedback demand even at the limit.
3. To make room, textures are shrunk farthest first, down to the mip their distance
   allows, before any near texture is held. Unseen textures go first, as today.
4. The near range follows memory: wide on a large card, narrower on a small one, never
   below the always-full set (own avatar, always-render avatars, camera focus).

Avatar ranking above is the mesh and skinning half of the same rule.
