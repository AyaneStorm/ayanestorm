# New renderer: shadows study (P4)

Status: study, nothing written. Decisions taken with the user on 15 November 2026:

- Shadow sources: sun or moon, projectors (at least LL's 2, more if the budget allows), and point lights.
- Soft edges: contact-hardening (sharp where things touch, softer with distance).
- Moving sun: both options are built and measured. Option 1: the shadow follows the sun every frame. Option 2: the shadow's sun direction moves in steps too small to see, so cached shadows survive between steps.
- The method must be very fast, good enough for local lights, and keep up with moving avatars.
- Shadow maps are computed once and reused by every consumer: lighting, transparency, water, probes, volumetric lighting.
- No ray tracing.

## What decides the method

1. **SL content.** Draw distance up to 1024 m, large flat triangles (prims, megaprims, terrain), lots of alpha-masked foliage and hair, user content never made with shadows in mind, crowds of up to 150 skinned avatars that move all the time.
2. **Hardware.** GT 1030 and 4 GB cards up to a 3080 Ti, Apple M1 to M4. VRAM is a hard limit.
3. **Real render rule.** Every shadow is correct in every frame. Reusing what has not changed is allowed: static content can stay cached. Methods that blend earlier frames (temporal accumulation, denoisers) are out.
4. **Reuse.** A shadow map must answer "is this point in shadow?" for **any point in space**, not only for the pixels on screen:
   - volumetric lighting samples it along the view ray, in the air;
   - Mac OIT samples it at each transparent layer's own depth;
   - water and probes sample it from places the camera's pixels do not cover.

   A screen-space shadow mask cannot do that. So the shadow maps themselves are the shared result, and one shader function samples them for everyone.
5. **What the renderer already has.** GPU culling and occlusion, the depth pyramid in the registry, skinned positions computed once and shared by every view, and graph passes that can be replaced.

Two filters follow from this:

- A method that changes the shadow's projection every time the camera moves can never reuse a cached map.
- A method whose map only exists where screen pixels asked for it fails rule 4.

## Candidates compared

### Cached cascades (chosen)

The view is cut into 3 or 4 depth ranges, each with its own shadow map covering a fixed area around the camera.

- **Caching:** each cascade keeps a static map of what does not move. When the camera moves, the map shifts and only the strip that came into view is drawn. Each frame the static map is copied into the working map, then everything that moves is drawn on top.
- **Moving avatars:** they are drawn every frame anyway, into the cascades they touch. Their cost depends on the avatars alone, never on what they walk past.
- **Reuse:** every point inside a cascade's area has an answer, on screen or not. Volumetrics, transparency, water and probes near the camera all read the same maps.
- **Local lights:** the same static-plus-moving scheme works for every projector and point light map.
- **Cost:** a copy per cascade, the moving content's draws, and the strips. The copy keeps one map per cascade for every consumer, instead of every consumer sampling two maps (twice the taps, which hurts volumetrics most).

### Virtual shadow maps with SMRT (not chosen)

One huge map cut into tiles (pages). Only the pages that screen pixels need get memory, and each page is cached until something in it changes. SMRT (shadow map ray tracing, no hardware ray tracing) marches short rays through the map for soft edges.

- **Sharpest result**, and the best caching for a still scene.
- **Moving avatars:** every page a moving avatar touches is redrawn. In a 150-avatar crowd that is a large share of the visible pages every frame. Avoiding it means a separate static and moving cache per page, which is the cascade scheme again with page tables added.
- **Reuse:** pages exist only where screen pixels asked for them. Volumetrics, transparent layers, water and probes each need their own page marking pass, or fall back to coarse pages. That breaks rule 4 unless every consumer is wired into the marking.
- **Local lights:** possible, but each light adds its own page tables and marking.
- **Work:** page marking, page table, culling per page (small groups of triangles), on two APIs.

**Not chosen**: it loses on moving crowds and on reuse, which are the two things this design is for.

Reviewed against Nanite (SIGGRAPH 2021), where virtual shadow maps were born: their speed comes from culling clusters per page and drawing every shadow view in one multiview chain. Our renderer has clusters too, so the 'work' argument above is weaker than it reads. The choice stands on the other two reasons: pages redrawn under moving avatars, and pages existing only where screen pixels asked. Nanite's multiview is taken for the cascades and the atlas (see the P4 plan's open items). Quantized SDSM (below) gives cascades most of the sharpness.

### Other recent methods

| Method | Verdict |
|---|---|
| **Bend Studio screen-space shadows** (2023) | **Kept.** A very fast trace towards the sun through the depth buffer, in an order that makes it cheap. It catches small detail and contact points no shadow map resolves. It only works for opaque pixels on screen, so it is a refinement on top of the maps, not a source other consumers read. |
| **Distance-field shadows** | **Later option** for shadows beyond the last cascade. Fields for static meshes are built on the workers. No skinned avatars, so only as an addition. |
| **Frustum-traced / irregular z-buffer** (2016) | Pixel-exact hard shadows, but they rely on conservative rasterization, which I don't think Metal has. They are costly in large scenes, and they only work for screen pixels. **Rejected.** |
| **Moment shadow maps** | Filterable, with less leaking than VSM, and they handle translucent casters. 4 times the memory of 16-bit depth. **Held** for tinted shadows later. |
| **Precomputed voxel shadow trees** | Computed for one fixed sun direction, while SL suns move and content is edited live. **Rejected.** |
| **Tetrahedron shadow maps** | 4 views per point light instead of 6. **Measured** against cubes with unseen faces skipped. |
| **Neural shadow mapping** | Needs earlier frames and heavy compute. **Rejected.** |

### Older methods from the list

- **Warping** (PSM, LiSPSM, TSM, CSSM, SPPSM, LogPSM, RTW): the projection follows the camera, so nothing can be cached. Edges shimmer, quality breaks looking towards the sun, and LogPSM and RTW bend SL's large triangles. **Rejected.**
- **Filterable maps** (ESM, VSM, convolution SM, SAVSM, VSSM): leaks where shadows overlap (everywhere in SL), and 2 to 4 times the memory. **Rejected.**
- **SMSR** (silhouette revectorization): smooths staircase edges in the far cascades, screen-space, cheap. **Optional pass, measured.**
- **SSSS** (screen-space soft): blurs wide shadow edges in screen space. **Measured** as a speed-up for wide PCSS edges, on screen pixels only.
- **DPSM** (bends large triangles), **SSSM** (complex; SMSR gives most of it), **MDSM** (replaced by slope and normal-offset bias), **FSM** (not relevant), **FIV** (lighting, not shadows). **Rejected.**
- **DSM, AVSM, DASM** (deep and volumetric): coloured shadows through glass and hair, the same idea as Mac OIT's moments. **Held** for tinted shadows later; the first version cuts alpha as LL does.

## The design

### Sun or moon

1. **4 cascades** (fewer on small budgets), centred on the camera, each snapped to whole texels. **Fixed reach** (user, 26 November 2026): the draw distance up to 512 m, split between logarithmic and even. Quantized SDSM (fitting the splits to the depth range on screen) was built and dropped: the reach changed as the camera turned or a head moved, cascades jumped size, and self-shadowing flickered (glossy earrings flashed; they stopped with shadows off). The cost is some sharpness in near cascades indoors.
2. **Static cache per cascade**, scrolled when the camera moves, copied into the working map each frame, with moving content drawn on top. How (26 November 2026):
   - **Each frame:** a shift pass copies the cache into the working map, moved by the whole texels the cascade scrolled, writing empty depth in dirty tiles; static casters are drawn into the dirty tiles; the working map is copied back as the new cache; moving casters are drawn on top. Two map copies a frame (about 0.1 ms at 2048), no extra memory, and the lists and readers stay as they are. A wrap-around map was weighed: it saves the copies but puts clusters across its seam in the lists more than once, which cannot be sized exactly.
   - **Window in whole texels** of a world-anchored grid across the light, and **depth from a world origin** along the light, stepped by a quarter of the cascade: a depth step redraws that cascade.
   - **Quantized size:** a cascade's radius moves in steps of a quarter octave and keeps its size until the fit leaves that step; a new size redraws it.
   - **Dirty tiles** (64 texels square): marked by the fit for new strips, a whole cascade on a new size, light step or depth step, and by world spheres the store reports: an instance added, removed, changed, or starting or stopping to move. An instance is moving while bound to a skeleton or for a short while after its transform last changed.
   - **Static casters** are culled into the shadow lists only where their bound touches a dirty tile, cleared and redrawn there; **moving casters** every frame, on top of the copy, into the working map.
   - **One LOD per instance for every view, as OpenGL's shadows** (changed 26 November 2026, after a per-cascade LOD closed windows that SL's low LODs fill in): a `lod select` job of the cull shader picks each instance's LOD before any cull reads it, and marks a static instance's tiles dirty when its LOD changes, so the cache follows what the camera shows in the same frame.
3. **One cull and one draw for all shadow views (agreed 26 November 2026):** shadow views are an array (4 cascades, and later every local light map). One compute dispatch tests each instance against every view and appends its clusters to one shared list, each entry tagged with its view. One draw follows: the vertex shader reads the tag and sends the cluster to that view's layer (`gl_Layer` from the vertex shader, core in Vulkan 1.2; Metal: `render_target_array_index`). A local light map is a rectangle of the atlas, cut by 4 clip planes, so any number of maps share the same draw. This replaces *multiview*, which sends every triangle to every layer, so the sharpest cascade would rasterize all the widest one sees. Planar reflection and probes later add entries to the same view array. Skinned avatars use the shared skinned positions, never skinned again: the shadow cull runs before the skinning pass and asks for deformation with the main cull's first-wins request, and an avatar's LOD is chosen once, before culling. Every caster takes its instance's one LOD (item 2).
4. **Moving sun:** options 1 and 2 behind a setting, measured.

### Projectors and point lights

1. **One shared atlas:** a big depth texture holding many small maps, one per projector and a cube (or tetrahedron) per point light. Each map's size follows the light's size on screen and the budget tier.
2. **Static cache per map**, the same scheme as the cascades. A light whose static content is unchanged costs only its moving content: the avatars in its range.
3. **Point lights:** only the faces that see something visible on screen are drawn.
4. **Selection:** by importance (brightness × size on screen, nearest first). The count is capped by the budget. LL's 2 projectors are the minimum on every tier.
5. **Volumetrics:** they read this atlas too, so a light's map is kept while it lights visible fog, even if its surfaces are off screen.

### Results in the registry, made once per frame

| Result | Read by |
|---|---|
| `sun shadow`: cascade array, matrices, split depths | lighting, Mac OIT, water, probes, volumetrics, terrain |
| `local shadow atlas`: atlas texture, per-light matrices and rectangles | the same, per light |
| `shadow min mips`: min-depth mips of both, for PCSS's blocker search and volumetrics' fast stepping | PCSS, volumetrics |
| `sun shadow mask`, per view: PCSS + Bend screen-space shadows for opaque pixels | the opaque lighting resolve |

- **One sampling function:** `asr_shadow_sun(worldPos, normal)` and `asr_shadow_local(light, worldPos, normal)`, with bias and PCSS inside, shared by every shader. A consumer that is not an opaque pixel calls them directly.
- **Tracked by the graph:** a consumer that is turned off stops nothing else, and no consumer makes its own shadow.

### Soft edges

- **PCSS (built 26 November 2026, `asr_shadow_sun()`):** the blocker search reads the map itself, 8 gathers: the 4 by 4 texels around the point and 4 on a circle as wide as the widest edge (20 m of blocker height). No blocker: lit, no filter; all blocked: shadow, no filter (interiors). Otherwise the blockers' mean height sets the radius: the 4 by 4 box up to 2 texels, then 16 gathers on a Vogel disc, blended between the two. The disc turns by a hash of the world texel, the same in every frame. **No min mips:** rebuilding them for four 2048² maps every frame (moving casters change them) reads 64 MB a frame, over 1 ms on a GT 1030, and costs VRAM; the early-outs make the direct search cheaper than the old 4 by 4 box on most pixels. Cascade edges blend into the next cascade over 8% of the width.
- **Contact shadows (cooperative, 27 November 2026, `contact shadows` pass, `asrContactShadowsC.glsl`):** Bend Studio's screen-space shadows, written from their description. Rays towards a directional light all run through its point on screen; around it the screen is cut into 4 sectors, each into bands of 128 rows, and one 128-thread group follows one ray across one band, reading the depth along it once into shared memory. Each pixel compares up to 128 of those depths, only those within 6 cm of it in the world (its end point projected; the ray's 1 / distance is linear along its screen line). Exactly one ray owns each pixel (the nearest, in exact integer arithmetic), so the mask is the same in every frame whatever order the groups run in. A 1-thread setup makes the work list and the indirect dispatch on the GPU from the exposure pass's light direction (`ContactShadowsRef`). The mask (`sun shadow mask`, R8, set 1 binding 1) is written only within 12 m; the resolve reads it only there, from the same depth texel. Groups with no pixel in range stop after one read. Measured (contact test, city, street): 3080 Ti 0.05 to 0.11 ms; M4 0.09 ms (city) to 0.45 to 0.48 ms (close-up, street), after the 64-bit integer work that Apple GPUs emulate was cut to the far-light case. Replaces the 12-step trace that ran in the resolve. Debug view 12 shows the mask; `asrenderer_test --contact-test` puts small casters (2 cm pole, 3 cm cube, 1 cm slab, 3 mm rod 2 cm up) on a platform 200 m up, with `--camera EX EY EZ TX TY TZ` for a still camera. Faded out where the light grazes the side seen, where the trace finds its own surface. **Since 28 November 2026** (hair opacity maps, below): the reach is cascade 0's bias, 3.5 of its texels (about 1.75 cm at 2048², at most 6 cm), the gap the maps leave above each receiver; the 6 cm avatar alpha push it once covered is gone, and with the fixed 6 cm reach faces in a high sun took hard patches under the brows, nose and lips (user's capture, 27 November 2026). Blended surfaces are not in the depth and cast no contact shadow.
- **Light size:** the real sun's disc (0.00465 rad) for the sun and moon, not EEP's drawn size, which can be many times larger and would blur every shadow; a projector's focus and ambiance settings, a point light's radius.
- **Casting alpha (built 26 November 2026):** a masked texel at or above its cutoff is solid, a blended one counts by its alpha, and either casts from 0.5 up, read one mip sharper than the shadow texel. OpenGL's half-dense columns below 0.88 became grey blotches crawling over faces. What the masked draw reads is kept per texture slot and cascade (`GpuShadowViews::demand`) and added to the texture feedback every frame (`asrShadowDemandC`), so a texture only a shadow shows keeps its size. A texture or material change redraws the cached tiles of its casters.
- **Wide edges:** screen-space blur (SSSS) measured against wider PCSS; the faster one wins at equal look.
- **Volumetrics:** they use plain hardware PCF (one tap, filtered), since fog hides edge detail.

### Memory

- Cascades: 16-bit depth where the range allows. At the default tier, 4 × 2048² × 2 bytes = 32 MB working + 32 MB static cache. On small budgets, 4 × 1024².
- Atlas: 4096² × 2 bytes = 32 MB working + 32 MB static at the default tier, 2048² on small budgets.
- Min mips: about a third more on top of what they are made from.
- Everything comes from the shadow category of the VRAM budget, and shrinks last in the degrade order.

## Next and open (27 November 2026)

Next, in order (each measured in a reproducible asrenderer_test scene before a viewer build):

1. **Done 27 November 2026, tested in `asrenderer_test` on Vulkan and Metal; viewer unbuilt** (see Soft edges). **Cooperative contact shadows** (Bend Studio's screen-space shadows): pixels along a line towards the light share their depth reads in workgroup memory, sample counts fixed at compile time. Replaces the per-pixel 12-step trace in the resolve; the resolve reads its mask. Agreed 27 November 2026: 128 samples, each pixel counting only those within 6 cm in the world (the avatar alpha push), so close-ups to about 0.3 m keep hair shadows; the dispatch list (light's screen point, up to 8 indirect dispatches) made on the GPU from the exposure pass's light direction; the mask is R8 (`sun shadow mask`); snapshot tiles overlap by 128 pixels.
2. **Blocker search weighted by distance:** in PCSS, near blockers bound the influence of far ones, so a tree's height does not blur the contact shadow of a chair leg.
3. **Per-cascade caster counts** in asrenderer_test (triangles of moving casters drawn into more than one cascade, with `--avatar-sweep`), then decide on **opaque depth reuse across cascades**: an opaque caster wholly inside a finer cascade drawn only there, its depth resampled into the coarser cascade's overlap (destination texel centres, depth range conversion, slope correction, no alpha casters). Worth it only if those duplicates are many: static casters are cached and receiver culling already keeps most avatars out of unread cascades.
4. Then the smaller items: casters smaller than a texel skipped per cascade, far cascades' moving casters at the update interval, staggered static updates, simple stand-in shadows for distant avatars (a setting).
5. Win back the resolve cost: Mac 1.1 ms before PCSS, 1.55 ms now (the box always runs plus 4 search taps).

## Proposed 27 November 2026: avatar shadow maps and the speed items (awaiting the user's review)

Why: the avatar-only rules (alpha casters 6 cm back, contact shadows standing in for strand shadows, the patches they leave on faces in a high sun) all work around one cut: hair is cut at alpha 0.5 into a yes/no depth map, and strands far thinner than a texel become an on/off pattern that pulses as the avatar moves. Per-avatar maps were considered and dropped: cascade 0 already has texels of about 5 mm (2048² over the nearest 10 m), a 1024² avatar map about 2 mm, and strands are a fraction of a millimetre: no affordable resolution resolves them.

**A. Hair opacity maps (agreed 27 November 2026; done 28 November 2026, tested in `asrenderer_test` on Vulkan and Metal, viewer unbuilt):** `sun opacity` and `opacity cache` (RG16F, same size as the depth maps: 128 MB at 2048², 32 MB at 1024²), `asrShadowAlpha.glsl` (depth where coverage is 0.9 or more, else optical depth and its depth sum), `shadowOpacity()` in `asrShadow.glsl` (4 bilinear taps, a texel out at least, wider with height). `--contact-test` has a sheet with half-millimetre holes 3 cm up: an even half shadow, smooth edges, Vulkan and Metal alike. Cost: M4 resolve +0.1 ms, shift +0.1 ms. Also fixed: the first-frame layout validation messages (`TargetImage::fresh`), and `asrShadow.glsl` was missing from the shader build's dependencies.
- Alpha casters (hair, lashes, lace, leaves) are no longer cut at 0.5 in the shadow maps. They draw into an extra target per cascade layer: the light let through (multiplied, from the texture's mip-filtered alpha) and the depth of the nearest such caster.
- A receiver beyond that depth is darkened by that amount; opaque casters stay in the depth map with PCSS. Soft, stable strand shadows at any resolution, no avatar rule.
- Removed: the 6 cm alpha push, contact shadows' role for hair, and with them the face patches.
- VRAM: 4 x 2048² x 2 bytes = 16 MB (4 MB low tier). Base for tinted shadows later.

**B. Contact shadows** keep one rule for every receiver (the grazing fade); no avatar exception. Reach and the 128 samples stay.

**C. Skin shading** (planned, after probes) softens every shadow on skin; until then avatar shadows look as soft as PCSS makes them.

**D. Speed items, in the order measured:**
1. **Done 28 November 2026, Vulkan and Metal (viewer unbuilt).** Per-cascade caster counts (`--audit` prints triangles per cascade, static and moving, and those drawn into a further cascade again; `--avatar-sweep --audit` too). 80 avatars, 3080 Ti: 41.2 M moving triangles in cascade 1, 28.1 M in cascade 2, 27.4 M of them twice; `shadow moving` 9.9 ms of a 19.8 ms frame. Cause: the blend band and the coarse filter (PCSS search up to 24 texels) read a coarser cascade inside the finer one's window, and the receiver tiles (6.75 m in cascade 2) cannot tell. **Depth reuse taken, without a copy:** a moving cluster wholly inside a finer cascade's window (a coarse texel to spare) is drawn there alone (`finestReader()` in the cull), and every coarser read inside that window reads the finer map at the same world points (`shadowGather()`, and the opacity taps), depth converted. Off for a cascade where a visible pixel inside its window lies past its depth range (`GpuShadowViews::deep`, from the receivers pass). Result: cascade 2 1.0 M, twice 0.3 M, `shadow moving` 5.8 ms; no extra VRAM or pass; M4 resolve unchanged (2.10 ms). Checked: static shadows identical with the redirect on and off; a probe (redirected reads forced dark) darkens exactly the coarser cascade's reads next to the finer one. M4 crowd: `shadow moving` 19 to 32 ms for 41 M cascade-1 triangles: next, shadow LOD (casters at the LOD their cascade's texel needs).
   **Shadow LOD, done 28 November 2026, Vulkan and Metal (viewer unbuilt).** Each skinned binding gets a **shadow twin** (`SceneStore::bindTwin()`): a second binding of the same chain on an instance of the store's own (`INSTANCE_SHADOW_TWIN`, `SceneQueue::reserveInstance()`), with storage only for the LODs below the finest; made, updated and removed with its main binding. In the bind pass (`shadowLod()` in `asrSceneSkinC.glsl`) the twin takes the coarsest LOD whose mean triangle width (`GeometryData::triangleWidth`, measured in `buildGeometry()`, `GpuBinding::lodWidth`) is at most 2 texels of the finest cascade holding the avatar, never finer than the camera's LOD; nothing when that is the camera's LOD (the main instance casts). The camera never draws a twin; shadows skip a main instance whose twin took a LOD. Off screen the twin still casts (before, off-screen avatars cast nothing). A width estimated from bound and cluster count made a limb's triangles look half again too wide, so widths are measured. Test crowd (4 LODs per face, 3080 Ti): cascade 1 41 M to 10.4 M triangles, `shadow moving` 5.8 to 1.5 ms, skinning +0.6 ms, frame 22.0 to 18.1 ms, VRAM +150 MB (tracked 2527 to 2677 MB). M4: `shadow moving` 19 to 32 ms down to 6.7 to 10.7 ms. Image with twins on against off (`--avatar-sweep --screenshot`, ground under the crowd, posed at time 0): 0.016 % of pixels differ by more than 8 of 255, none by more than 32. Seen then, not shadows: the M4 crowd's camera passes (`draw early` 26 ms, `groups late` 33 ms).
2. Casters smaller than a texel skipped per cascade.
3. Far cascades' moving casters at the update interval; staggered static updates.
4. Stand-in shadows for distant avatars (a setting).
5. Resolve cost back (item 5 above): the box only where the blocker search needs it.
6. Distance-weighted blocker search (item 2 above), with its cost measured alongside 5.

Each batch: a reproducible asrenderer_test scene and timings (Vulkan and Metal) before a viewer build. Test scene for A: an avatar-sized head with strands, sun at 30 and 88 degrees, view 12 and the new view for the avatar map.

Fixed with item 1: debug views 6, 7 and 9 (the image with one term left out) skipped tone mapping like the colour-coded views, so they showed darker than view 0 (view 9 at half the mean brightness): readings of view 9 before 27 November compared unlike images. The shadow dirty spheres lacked the device address flag, and the bloom atlas was never destroyed (Vulkan validation).

From the first viewer check of item 1 (27 November 2026): on a face close-up in a sun near its highest, large soft grey patches under the eyes, along the nose and over the upper lip and chin (not with a lower sun): the face's own shape found by the 6 cm trace, hard-edged. Addressed 28 November 2026 by the hair opacity maps and the bias-sized reach (below); viewer check pending.

Fixed 28 November 2026: Vulkan validation reported, once at start, the sun shadow array and its cache used in a layout they were never moved to (a new image's first barrier now starts from no layout).

Not taken: a dithered cascade transition (needs frames blended), ray-traced shadows with temporal denoising (hardware and the real render rule).

Open: **striped rectangle on flat paving** at one plaza, at some camera positions only. One-pixel screen columns shadowed inside a prim's outline, in every cascade, even with the sun overhead and with `ASRendererShadowInterval` 0. Ruled out: contact shadows (view 9), the cache (interval 0), LOD (RenderVolumeLODFactor 4), an alpha layer (view 3), the normal rules (view 10 blue). Suspect: two near-coplanar surfaces alternating in the camera's depth test, the lower one shadowed by the upper. To settle with a two-surface test scene and view 4 (fixed: views 1, 2, 4 clipped white before 27 November).

## To measure (P4)

- Cascade cost with and without the static cache: standing still, walking, turning, flying, and in a 150-avatar crowd.
- Moving sun: option 1 against option 2, in the street scene and in the crowd.
- PCSS alone against PCSS + screen blur.
- SMSR on and off.
- Atlas with 2, 8, 16 local lights; cubes against tetrahedra.
- Volumetrics reading both results: cost per sample with the min mips.
