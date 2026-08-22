# AyaneStorm Volumetric Lighting — End-to-End Transmittance Plan

## Read this first

This is a scoping document for a NOT-YET-STARTED feature, written at the end
of a prior session so a fresh session (with no memory of that conversation)
can pick this up. No code has been changed for this specific work. It builds
on top of the already-shipped, working `ASVolumetricLighting` module — see
`doc/ayanestorm-volumetric-lighting-plan.md` for that feature's full history,
architecture, and hard-won lessons (in particular: check `AyaneStorm.log` for
shader link failures FIRST whenever a `.glsl` change seems to have no effect,
and verify the build's staged `app_settings` directory actually matches
source before concluding a shader-only change did nothing — both bit the
original implementation multiple times).

Do not start writing code from this document without first re-reading the
current state of the files it cites — they may have moved since this was
written (2026-08-22).

## The goal

Volumetric extinction (`RenderVolumetricLightingExtinction`,
`ASVolumetricLighting::getExtinction()`) currently only WEIGHTS the
in-scattered light added on top of the scene:

```
C_out = C_scene + V_scatter * exp(-extinction * distance)
```

The physically complete model also attenuates the scene itself along the
view path:

```
C_out = C_scene * T + V_scatter,  where T = exp(-extinction * distance)
```

Right now the `* T` term on `C_scene` does not exist anywhere in the
codebase. This is item 1 on the "Remaining transferable ideas from the Unity
references" list in the main plan doc, called out there as "the largest
physically meaningful improvement" but explicitly flagged as needing separate
validation because it touches the full scene/alpha/water compositing
equation.

**Motivating context**: in the session that produced this doc, the user
reported the sun disc looking washed out/hard to see at certain
intensities/angles. Investigation traced this to the volumetric composite
being a pure depth-unaware additive blend with no scene attenuation — adding
scene transmittance is the physically correct fix for that class of
overexposure (as opposed to clamping intensity or masking near the sun disc,
which would be a band-aid). That screenshot ultimately looked fine on
inspection, so this is not a confirmed bug fix, but it is the most likely
real-world visible benefit of doing this work.

## Why this is a big, atomic change (not incremental)

Two structurally separate pieces are both required for a genuinely complete
result. Do not implement only one and consider the feature done:

1. **Opaque composite** (`ASVolumetricLighting::renderPass()` in
   `indra/newview/asvolumetriclighting.cpp`) needs to multiply the EXISTING
   `screen` contents by `T` before/while adding scatter.
2. **The transparency atlas** (`sTransparencyAtlas`, built by
   `renderTransparencyAtlas()` from `asVolumetricAtlasF.glsl`) needs to start
   carrying transmittance forward so every consumer shader can attenuate its
   own scene-color term, not just add foreground scatter on top of it
   unattenuated.

Water's refracted contribution (the dominant part of its volumetric
appearance) depends on item 1 being done first: `waterF.glsl` samples
`screenTex`, which is the already-composited opaque scene — so if item 1 is
skipped, water's refracted share stays physically incomplete no matter what
the atlas carries. See `waterF.glsl:93-96` and `:371-374` for the existing
comment describing this relationship (line numbers as of 2026-08-22, verify
before use).

If the atlas's texture format/channel count changes (which it will need to,
to carry `T`), **every consumer of that atlas must change in the same
commit/session** — a stale consumer reading the old 1-value-per-tile format
against a new-shaped atlas will read garbage, not just "miss out on
transmittance." This is the reason this is not a slow, one-file-at-a-time
migration.

## Facts gathered during scoping (verify line numbers before use — code may have moved)

### 1. Opaque composite mechanism

- Blend state: `asvolumetriclighting.cpp` sets
  `gGL.setSceneBlendType(replace ? LLRender::BT_REPLACE : LLRender::BT_ADD)`
  in the `draw_composite` lambda inside `renderPass()`. Normal mode uses
  `BT_ADD` = `(ONE, ONE)`, alpha write masked off
  (`gGL.setColorMask(true, false)`) because additive alpha blending was
  previously found to corrupt later HDR post-processing (see main plan doc,
  "Post-build fixes" item 3, and the round where mode 2 read as "all white").
- `asVolumetricCompositeF.glsl` (the composite fragment shader) never reads
  the destination/`screen` texture at all today — it only samples
  `emissiveRect` (the scatter source, i.e. `sVolumetricTarget`). GL blend
  equations alone cannot express `dst = dst*T + src` in one call; that needs
  either a shader that reads the destination as a texture (impossible while
  it's simultaneously the bound framebuffer draw target — same constraint
  that motivated the ping-pong scratch textures in `renderTransparencyAtlas`,
  see `asvolumetriclighting.h`'s comment on `sAtlasIntegralTex`), or two
  separate blend passes (one multiplicative to scale `screen` by `T`, one
  additive to add scatter).
- **Recommended mechanism**: copy-first ping-pong, mirroring the existing
  `sAtlasIntegralTex[0]/[1]` pattern already proven in this codebase
  (`renderTransparencyAtlas()`, `asvolumetriclighting.cpp`). Concretely:
  1. Before the current composite draw, copy/blit `screen`'s current
     contents into a new scratch `LLRenderTarget` (full resolution, same
     format as `screen`).
  2. Modify `asVolumetricCompositeF.glsl` to take a second sampler bound to
     that scratch copy, plus a transmittance value/texture (see atlas section
     below for where `T` comes from), and output
     `frag_color = vec4(scene_copy.rgb * T + scatter.rgb, 0.0)`.
  3. Write that result into `screen` with `BT_REPLACE` instead of `BT_ADD`
     (since the shader now does the "add to existing" arithmetic itself,
     reading from the *copy* rather than relying on GL blending against the
     live target).
  - This is very likely the single most GPU-expensive part of the whole
    feature once added (a new full-resolution render target + a full-screen
    copy every frame) — consider whether the copy can be done at
    `sVolumetricTarget`'s resolution (half-res by default) instead of full
    screen resolution, accepting some softening of the attenuation edge, the
    same tradeoff already made everywhere else in this feature.

### 2. Atlas consumers (5 files, all must change together)

All five share the same shape: a local `asVolumetricForeground`/
`asVolumetricWaterForeground` function samples 1-2 tiles from
`asVolumetricAtlas` and returns `light_color * clamped-scatter`
(confirmed: `asVolumetricAtlasF.glsl`'s file header explicitly documents
every tile as "the exact same final...light_color * clamped-scatter value" —
no transmittance term currently exists separately from that product). Files
and call sites (verify line numbers, this is as of 2026-08-22):

- `alphaF.glsl` — function ~lines 79-110, call ~line 390:
  `color.rgb += asVolumetricForeground(vary_position)`. `color.rgb` at that
  point is the final lit, fogged, `final_scale`-multiplied color with
  nothing further applied before output.
- `pbralphaF.glsl` — function ~lines 73-89, call ~line 275:
  `color.rgb = color.rgb * final_scale + asVolumetricForeground(pos.xyz)`.
  Same situation.
- `materialF.glsl` — function ~lines 102-118, call ~line 487. **This is the
  trickiest consumer**: it branches into `EXACT_OIT` (line ~489:
  `color * final_scale + volumetric_foreground`), `AVBOIT` (lines ~500-508,
  where `avboit_color = color * final_scale` gets its OWN separate
  alpha-recomputation/clamp treatment — `avboit_alpha` — before
  `avboit_color += volumetric_foreground`), and a plain path. Transmittance
  must be applied to `color * final_scale` (and the `avboit_color` rescale)
  in every branch, but must NOT be applied to `volumetric_foreground` itself
  (that's already-final scatter, not scene color). Read this file in full
  before editing; do not assume the three branches are structurally
  identical.
- `fullbrightF.glsl` — function ~lines 54-68, call ~line 156, guarded by
  `#if !defined(IS_HUD)`. `color.rgb += asVolumetricForeground(pos)`.
- `waterF.glsl` — function ~lines 100-116, call ~line 376:
  `color += asVolumetricWaterForeground(pos) * min(1.0, df2.x) * fade`.
  Structurally different from the others: `color` here is a mix of `fb.rgb`
  (a sample of `screenTex`, i.e. the ALREADY-opaque-composited scene behind
  the water — see the comment at lines 93-96/371-374 explaining that
  `screenTex` already carries volumetrics through the refracted share) and
  reflected radiance. Only the Fresnel-reflected share routed through
  `asVolumetricWaterForeground` needs local transmittance handling here; the
  refracted share's transmittance comes for free once item 1 (opaque
  composite) is fixed, since it flows through `screenTex`.

**Atlas format change needed**: since no consumer can currently reconstruct
`T` from what the atlas stores, a new channel or second atlas texture must
carry raw transmittance forward per-tile. `asVolumetricAtlasF.glsl` already
has a precedent for a second MRT output (`integral_out`, `layout(location =
1)`) — but that one is explicitly documented as private ping-pong scratch,
"never sampled by anything outside `renderTransparencyAtlas()`" (see
`asvolumetriclighting.cpp`'s comment above `sAtlasIntegralTex`'s declaration
in the header) — it cannot be repurposed for this without breaking that
existing contract. A genuinely new, consumer-facing channel/texture is
needed. Options to weigh when designing this:
- Pack `T` into the atlas's existing RGBA16F alpha channel (currently unused
  — every tile write is `vec4(..., 1.0)` or similar with alpha not consumed
  by any reader). Cheapest, no new texture, but only works if this is the
  ONLY thing ever wanting the alpha channel; check nothing else expects to
  add a use for it in the atlas later.
- Add a second full `LLRenderTarget` (like the local-light target), same
  resolution as the atlas, single-channel (`GL_R16F`), written as part of the
  same `renderTransparencyAtlas()` MRT draw (there's already a working
  3-attachment-capable FBO management pattern there, though currently only
  2 attachments are used — extending to 3 is the more isolated, more
  future-proof option but costs more VRAM/bandwidth).

### 3. FSExactOIT / FSAVBOIT

Confirmed via full-file review: neither `fsexactoit.h/.cpp` nor
`fsavboit.h/.cpp` reference `asVolumetricAtlas`/`asVolumetricEnabled`/
`bindTransparencyAtlas` anywhere. They don't need to — atlas binding happens
at the C++ drawpool layer (`lldrawpoolalpha.cpp`, `lldrawpoolsimple.cpp`,
`lldrawpoolwater.cpp`, all calling `ASVolumetricLighting::bindTransparencyAtlas()`
on whichever shader object is passed in), and the shared `.glsl` files
(`alphaF.glsl` etc.) call `asVolumetricForeground()` unconditionally BEFORE
branching into `EXACT_OIT`/`AVBOIT`/plain output. So these two renderers
structurally inherit whatever the shared source does, with the one exception
already noted: `materialF.glsl`'s `AVBOIT` branch has bespoke alpha math that
must be threaded through explicitly (see section 2).

## Suggested build order

1. **Atlas format change first**, as one atomic commit/session:
   `renderTransparencyAtlas()` + `asVolumetricAtlasF.glsl` (produce and store
   `T` per tile, in whichever channel/texture is chosen) + all 5 consumer
   files (apply `T` to their own scene-color term, per the per-file notes
   above). Test with `RenderVolumetricLightingDebug` mode 10 (existing atlas
   debug visualization) extended to also show raw `T`, the same way earlier
   debug modes were added for the original feature — do not skip building
   a debug view for this, the original feature's history in the main plan
   doc shows how much debug infrastructure was needed to make an invisible
   raymarch bug diagnosable at all.
2. **Opaque composite second**: new scratch render target + ping-pong copy +
   modified `asVolumetricCompositeF.glsl`, in `renderPass()`.
3. Verify water's refracted share automatically improves once step 2 lands
   (per the `screenTex` dependency above), without further water-specific
   changes beyond the item-2 change to its Fresnel-reflected share.

Bump `ASVolumetricLighting::shaderCacheRevision()` before distributing a
build whose users retain shader caches, per the existing convention
documented at that function's declaration — but not for every intermediate
edit during active development (the comment there explains why).

## Explicitly out of scope for this change

Do not fold in any of the other "Remaining transferable ideas" from the main
plan doc (independent scattering/extinction coefficients, world-space density
shaping, true spotlight cone volumes, sky-path extinction policy) — those are
separate, independent quality improvements, not part of "end-to-end
transmittance." Keep this change scoped to making `T` real and applied
end-to-end; resist the temptation to redesign the scatter/extinction model
at the same time.
