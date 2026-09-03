# Exact OIT: lamp glow regression (fixed)

Author: chanayane@firestorm. Date: 2026-09-03. Status: **fixed, verified.**

## Symptom

A wall-mounted lamp's glass renders lit (glowing) in vanilla and in AVBOIT,
but unlit (dark) in Exact OIT. Regression introduced this session; did not
exist before the E-series work.

## Root cause (confirmed)

E2-B (commit `3b6985edcc`) added this filter to
`FSExactOIT::handleCapturedEmissives()` (`indra/newview/fsexactoit.cpp`,
around line 1150):

```cpp
auto drop_no_glow = [](std::vector<LLDrawInfo*>& v)
{
    v.erase(std::remove_if(v.begin(), v.end(),
        [](LLDrawInfo* d) { return !d->mHasGlow; }), v.end());
};
```

It relies on the plan's E2 trap text: "prims (LLVOVolume) only have
TYPE_EMISSIVE when glow > 0, so `mHasGlow` stays true for them". That claim
is false. `LLDrawInfo::mHasGlow` is:

- default-initialised to `false` (`llspatialpartition.h:143`,
  `llspatialpartition.cpp:4094`);
- written **only** by `LLParticlePartition::getGeometry()`
  (`llvopartgroup.cpp:922`), i.e. only for particle draws;
- never written by `LLVolumeGeometryManager::registerFace()`
  (`llvovolume.cpp:5807`), which creates every prim/mesh draw.

Full-tree grep for `mHasGlow` returns exactly those sites plus the filter.
So every prim emissive draw (lamp glass, any glowing alpha-blended face) has
`mHasGlow == false` and is dropped before `renderEmissives()` /
`renderPbrEmissives()` / rigged variants run. No glow node is captured for
it, the composite adds zero glow, the lamp is dark.

Why the other modes are fine: vanilla and AVBOIT select emissive draws by
`mVertexBuffer->hasDataType(TYPE_EMISSIVE)` (`lldrawpoolalpha.cpp:460, 912`)
and never read `mHasGlow`. `FSAVBOIT::handleCapturedEmissives()` has no such
filter.

Why particles still glow: E2-B's tagged block in `llvopartgroup.cpp:854-868`
sets `has_glow = mLastGlowNonZero` and stores it into `info->mHasGlow`, so
glowing particles carry `true`.

## Fix (choose A; B is the fallback)

### A. Mark prim draws as "may glow" at creation (recommended, 1 tagged line)

In `indra/newview/llvovolume.cpp`, `LLVolumeGeometryManager::registerFace()`,
in the `else` branch that creates a new `LLDrawInfo` (around line 5807,
right after `draw_info->mModelMatrix = model_mat;`), add:

```cpp
        // <AS:Chanayane> Exact OIT E2-B reads mHasGlow to drop emissive
        // draws whose glow is known to be zero. Only LLVOPartGroup computes
        // that; a prim draw only carries TYPE_EMISSIVE when its spatial group
        // has a glowing face (llvovolume.cpp ~6215), so treat it as "may
        // glow" rather than the default false, which dropped every prim glow.
        draw_info->mHasGlow = true;
        // </AS:Chanayane>
```

Only new-info creation needs it: the batched branch above reuses an existing
`info` that already carries the flag. Vanilla never reads `mHasGlow`, so this
changes nothing outside Exact OIT. Prim faces with glow == 0 inside a glowing
group still get drawn, and E2-A's `if (glow == 0.0) return;` in
`exactOITEmissiveF.glsl` / `exactOITPbrGlowF.glsl` keeps them from allocating
a node; that was already the pre-E2-B behaviour.

Also fix the wrong comment above `drop_no_glow` in `fsexactoit.cpp`:
replace the sentence "Non-particle emissive draws (LLVOVolume) only ever get
TYPE_EMISSIVE when glow > 0, so mHasGlow is already true for them and this
filter is a no-op there." with "Prim draws set mHasGlow = true in
registerFace() (llvovolume.cpp) so they always pass; only particle draws
with known-zero glow are dropped."

### B. Fallback if touching `llvovolume.cpp` is refused

Remove the `drop_no_glow` lambda and its four calls from
`FSExactOIT::handleCapturedEmissives()`. That restores pre-E2-B behaviour
(E2-A alone). Cost: glow-less particle draws are submitted again (vertex
work only; they still allocate nothing). Leave the `llvopartgroup.cpp`
tagged block in place; it is harmless.

## Applied

Fix A applied 2026-09-03: `draw_info->mHasGlow = true;` added in
`LLVolumeGeometryManager::registerFace()` (`llvovolume.cpp`, new-info `else`
branch, right after `draw_info->mModelMatrix = model_mat;`), tagged. Comment
above `drop_no_glow` in `fsexactoit.cpp` corrected to match. No shader
source changed; no shader cache revision bump needed. Verified in testing:
lamp glows again in Exact OIT, matching vanilla and AVBOIT.

## Verify

- Build, Exact OIT mode: the lamp glass glows again, matching vanilla.
- Particle emitter with glow 0 (smoke): Exact OIT debug mode 1 list depth
  unchanged from the E2-B build (draws still dropped).
- Glowing particles: still glow.
- No shader source changed under A or B: no shader cache revision bump.

## Corrections to the plan

`doc/ayanestorm-oit-performance-audit-plan.md` E2 "Traps" and the status-log
E2-B row assert prims keep `mHasGlow == true`. Both are wrong; the plan's
status log now records this as plan defect #3.
