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

1. **4 cascades** (fewer on small budgets), placed by **quantized SDSM**:
   - the depth range on screen, read from the depth pyramid, fits the splits;
   - the splits are rounded to steps and kept until the fit moves by more than one step, so the cache survives and edges do not shimmer;
   - each cascade is snapped to whole texels.
2. **Static cache per cascade**, scrolled when the camera moves, copied into the working map each frame, with moving content drawn on top. How (26 November 2026):
   - **Each frame:** a shift pass copies the cache into the working map, moved by the whole texels the cascade scrolled, writing empty depth in dirty tiles; static casters are drawn into the dirty tiles; the working map is copied back as the new cache; moving casters are drawn on top. Two map copies a frame (about 0.1 ms at 2048), no extra memory, and the lists and readers stay as they are. A wrap-around map was weighed: it saves the copies but puts clusters across its seam in the lists more than once, which cannot be sized exactly.
   - **Window in whole texels** of a world-anchored grid across the light, and **depth from a world origin** along the light, stepped by a quarter of the cascade: a depth step redraws that cascade.
   - **Quantized size:** a cascade's radius moves in steps of a quarter octave and keeps its size until the fit leaves that step; a new size redraws it.
   - **Dirty tiles** (64 texels square): marked by the fit for new strips, a whole cascade on a new size, light step or depth step, and by world spheres the store reports: an instance added, removed, changed, or starting or stopping to move. An instance is moving while bound to a skeleton or for a short while after its transform last changed.
   - **Static casters** are culled into the shadow lists only where their bound touches a dirty tile, cleared and redrawn there; **moving casters** every frame, on top of the copy, into the working map.
   - **LOD per cascade for static casters**, from the cascade's texel size, not the camera's distance: a cached shadow cannot follow the camera's LOD choice. Far cascades get coarse meshes. Skinned avatars keep their one LOD (they are moving content).
3. **One cull and one draw for all shadow views (agreed 26 November 2026):** shadow views are an array (4 cascades, and later every local light map). One compute dispatch tests each instance against every view and appends its clusters to one shared list, each entry tagged with its view. One draw follows: the vertex shader reads the tag and sends the cluster to that view's layer (`gl_Layer` from the vertex shader, core in Vulkan 1.2; Metal: `render_target_array_index`). A local light map is a rectangle of the atlas, cut by 4 clip planes, so any number of maps share the same draw. This replaces *multiview*, which sends every triangle to every layer, so the sharpest cascade would rasterize all the widest one sees. Planar reflection and probes later add entries to the same view array. Skinned avatars use the shared skinned positions, never skinned again: the shadow cull runs before the skinning pass and asks for deformation with the main cull's first-wins request, and an avatar's LOD is chosen once, before culling. Static casters take each cascade's own LOD (item 2); moving ones the main view's.
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

- **PCSS:** a blocker search on the min mips gives the blocker distance, then a PCF filter that widens with it. The sample pattern rotates per pixel but is the same in every frame, so nothing needs temporal smoothing.
- **Light size:** the sun's or moon's angular size, a projector's focus and ambiance settings, a point light's radius.
- **Wide edges:** screen-space blur (SSSS) measured against wider PCSS; the faster one wins at equal look.
- **Volumetrics:** they use plain hardware PCF (one tap, filtered), since fog hides edge detail.

### Memory

- Cascades: 16-bit depth where the range allows. At the default tier, 4 × 2048² × 2 bytes = 32 MB working + 32 MB static cache. On small budgets, 4 × 1024².
- Atlas: 4096² × 2 bytes = 32 MB working + 32 MB static at the default tier, 2048² on small budgets.
- Min mips: about a third more on top of what they are made from.
- Everything comes from the shadow category of the VRAM budget, and shrinks last in the degrade order.

## To measure (P4)

- Cascade cost with and without the static cache: standing still, walking, turning, flying, and in a 150-avatar crowd.
- Moving sun: option 1 against option 2, in the street scene and in the crowd.
- PCSS alone against PCSS + screen blur.
- SMSR on and off.
- Atlas with 2, 8, 16 local lights; cubes against tetrahedra.
- Volumetrics reading both results: cost per sample with the min mips.
