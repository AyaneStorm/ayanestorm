# AyaneStorm branch architecture audit

## Scope

Audit of `volumetric-lighting` against `origin/ayanestorm-dev`, including the
working tree, for merge-sensitive logic placed in Firestorm/Second Life-owned
files instead of AS-owned modules.

## Corrected during audit

- Procedural sun halo rendering, settings reads, timing, geometry, blending,
  and uniform uploads moved from `lldrawpoolwlsky.cpp` to `asproceduralsun`.
- Moon halo rendering and moon-disc uniform policy moved from
  `lldrawpoolwlsky.cpp` to the new `asmoonrendering` module.
- Partial sun/moon visibility policy moved from `llvosky.cpp` to
  `ascelestialtwilight`; the shared sky object retains only calls and the
  private geometry-generation hook.
- Legacy hardware-light influence and horizon-clamped direction calculation
  moved from `pipeline.cpp` to `ascelestialtwilight`.

## Appropriate integration hooks

- Volumetric resource allocation/release/composite calls in `pipeline.cpp`.
- Atlas binding calls in alpha, fullbright, and water draw pools.
- Shader load/unload/cache-revision calls in `llviewershadermgr.cpp`.
- Twilight source-selection calls at shader/shadow boundaries.
- Reserved moon uniform registration in `llshadermgr`.

## Remaining refactors

1. **Duplicated volumetric GLSL sampling:** `fullbrightF.glsl`, `alphaF.glsl`,
   `pbralphaF.glsl`, `materialF.glsl`, and `waterF.glsl` repeat the same atlas
   coordinate and interpolation implementation. Move it to one AS-owned
   fragment utility and attach it through an explicit shader feature. This is
   high value but requires shader-link validation across legacy, PBR, rigged,
   HUD, impostor, material, and water permutations.
2. **Atmospheric state upload:** `llsettingsvo.cpp` still expands source,
   direction, and energy selection inline. Return a complete AS-owned
   atmospheric state and leave only uniform uploads in the shared file.
3. **Preferences XML churn:** `panel_preferences_ayanestorm.xml` retains a
   large commented-out copy of controls moved to dedicated floaters. Delete
   that dead block rather than carrying it through upstream merges.
4. **Moon shader size:** `moonF.glsl` necessarily owns the final fragment
   composition today, but its AS phase/halo functions should be reviewed for
   extraction only if the shader feature mechanism proves reliable in item 1.

## Guardrail

Future changes should treat shared render files as call sites. Settings,
policy, calculations, render-state ownership, and repeated shader algorithms
belong in `as*.cpp/.h` or AS-owned shader utilities. Shared files should retain
only the smallest hook required by their private data or pipeline position.
