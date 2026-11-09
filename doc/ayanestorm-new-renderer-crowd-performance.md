# Rendering performance and smoothness in crowded scenes

## Scope and evidence

This report explains the mechanisms present in the local source checkout that reduce rendering cost and frame-time spikes, with particular attention to avatars and everything they wear. It describes behavior and data flow without reproducing code or identifying the application.

The investigation used application sources, rendering-engine sources, shader sources, ARCHITECTURE.md, and the rendering and performance entries of TASKS.md. This is a source audit, not a new performance measurement. Numbers explicitly described as recorded measurements come from existing repository notes or source comments; they are historical observations under particular conditions, not universal speed guarantees.

## 1. The overall strategy

The strongest explanation for crowd performance is the interaction of several mechanisms, rather than one rendering trick:

1. Limit how many avatars need their full geometry rendered every frame.
2. Replace the remaining avatars with cached pictures and avoid computing fresh poses when those pictures will not change.
3. Share one skeletal pose and a small set of attachment transforms across all objects belonging to the same wearer.
4. Let GPU shaders deform and position geometry, and let GPU compute generate most draw lists.
5. Avoid shading hidden surfaces and avoid repeatedly drawing the same opaque shadow casters into several cascades.
6. Keep unchanged objects, materials, meshes, and textures out of recurring CPU work.
7. Spread loading, rebuilding, eviction, and resource retirement across frames.
8. Overlap simulation and rendering without accumulating a queue of stale input.

These mechanisms address different limits. Impostors reduce recurring geometry and shading work. GPU anchors and incremental scene maintenance reduce CPU work. Upload staging, memory policies, and background jobs reduce hitches. Pacing and motion reconstruction improve responsiveness even when the average frame rate does not increase.

## 2. Avatar rendering is explicitly budgeted

### 2.1 A bounded group receives full rendering

The scene ranks avatars by distance from the camera. The user's own avatar is assigned priority ahead of the others and always belongs to the full-render group. The configured full-avatar count is clamped to at least one; the own avatar consumes a place in that group. The constructor default is **16 full avatars**.

Avatars beyond that ranked group, inside draw distance and eligible for display, become impostors. Their worn objects follow the same decision. The renderer does not keep drawing a distant avatar's detailed clothing and hair behind its replacement card. Special portal-related exceptions exist, so an agent should not assume that every avatar beyond the ordinary rank is unconditionally replaced.

The distance ranking occurs before individual view visibility decisions. It should not be described as selecting exactly the nearest 16 *visible* avatars: offscreen nearby avatars can occupy ordinary full-render ranks.

This caps the recurring cost of complete bodies, clothing, hair, jewelry, and their many material faces. It does not impose a fixed triangle ceiling: a single fully rendered avatar can still wear very expensive geometry.


### 2.2 Complexity limits are a separate protection

The full-avatar count and the avatar-complexity limit are different controls. The latter can replace an expensive avatar with a simplified silhouette and suppress its attachments, even when its rank would otherwise permit full rendering. A distant complexity-limited avatar can have an impostor of that silhouette.

The complexity calculation accounts for geometry estimates, texture cost, alpha blending, flexible geometry, lights, media, glow, shininess, and other rendering multipliers. Attachment surface area supplies another limit when complexity limiting is enabled. The rules include own-avatar exemptions, friend modes, explicit render exceptions, and blocking behavior. The constructor defaults are a complexity limit of **350,000** and the ordinary everyone mode; a zero complexity limit also disables the associated surface-area rejection.

Complexity is a compatibility-oriented estimate, not a GPU timer or an exact triangle budget. The implementation also keeps the calculation itself inexpensive: periodic refresh work is spread over **16 slices**, and primitive triangle estimates are cached by shape instead of rebuilding all four detail levels for every complexity refresh.


## 3. Impostors replace repeated avatar rendering with cached pictures

### 3.1 What is cached

An impostor is a small rendered picture of an avatar, including its visible worn geometry, displayed on a world-space card. Taking the picture requires rendering the avatar; reusing it requires drawing the card. This is a temporal cache of the avatar's appearance, not a reduced polygon model.

Each picture is **256 × 256 pixels**. Its capture framing uses a square with a **1.4 m half extent**. Captures keep opaque/masked geometry and blended geometry in their respective lists. The card uses the captured viewing axes and is positioned at the avatar's current center.

The fixed framing is a practical limit: exceptionally large worn geometry is not guaranteed to fit perfectly. The picture is also viewpoint-dependent, so arbitrary camera movement requires refreshing it.

### 3.2 Refreshing is adaptive and bounded

The normal refresh period starts at **0.15 seconds**, adds **one second for every 60 m of distance**, and stops increasing at **one second**. For example, at 30 m the nominal period is about 0.65 seconds. A picture becomes urgent sooner when the viewing direction changes by more than approximately **4 degrees**, the avatar center moves by more than **0.3 m**, or the complexity/silhouette state changes.

Only **two captures are admitted per frame**. Missing pictures and changed silhouette states have the highest staleness priority; significant movement or viewpoint changes receive a large urgency increase. Other refreshes are ordered by staleness. Ties are resolved deterministically.

An urgent picture is therefore eligible immediately, but it is not guaranteed to be captured in that exact frame if more urgent avatars exceed the two-capture budget. This bound prevents a camera turn or crowd arrival from causing every distant avatar to be rendered in full at once.

### 3.3 Atlas allocation avoids both unbounded growth and disappearing avatars

Only visible impostors allocate or refresh picture tiles. Offscreen cached pictures expire after **five seconds** and can be reclaimed sooner when visible avatars need their slots. Visible pictures are retained through long frames and refreshes.

The atlas starts with **64 tiles** and grows up to **256 tiles**, in at most four pages. The documented allocation range is approximately **32–128 MiB**. Growth preserves existing pictures through GPU copies. Card instance-buffer capacity grows to accommodate the cards rather than assuming a permanently small crowd.

An avatar awaiting its first picture, or beyond the atlas capacity, gets a neutral silhouette card. It does not disappear and does not silently fall back to full geometry. This maintains the rendering budget under overload. Previously captured cards remain usable while waiting for their next refresh.

### 3.4 Texture resolution follows the cached representation

Impostor attachments still contribute texture demand, but their requested resolution is limited by both their screen size and the number of pixels they can occupy inside a 256-pixel capture. This avoids two opposite problems: fetching large textures for a tiny cached picture, and leaving an avatar that was never rendered in full permanently blurred because none of its textures had meaningful demand.


## 4. Impostors also save CPU animation work

Using an avatar picture would save only part of the work if the application continued posing the skeleton and updating every attachment at display rate. The current path holds an impostor's last pose until its next picture is nearly due.

The pose scheduler uses the same distance-dependent period as the picture scheduler, with a **50 ms lead**. It requests fresh evaluation when picture age, pose age, movement, viewpoint change, or silhouette state make reuse unsafe. Offscreen impostors can retain poses subject to the pose-age rule; they are not simply frozen forever.

Animation start/stop synchronization continues before the pose-reuse decision. The animation state therefore remains current even while expensive joint evaluation is skipped. When evaluation resumes, it samples the current animation time rather than replaying every missed display frame.

The own avatar, avatars without an existing pose, explicitly posed avatars, and relevant portal exceptions bypass this reuse path. Remote owners outside draw distance plus a **32 m margin** are skipped by this pose scheduling pass, while the own avatar continues advancing animation events that affect simulator state.

The savings propagate beyond animation. If a wearer has neither moved nor received a new pose, its attachment anchors need no rewrite. Attachment centers can also retain their previous values when their anchors are unchanged.

Recorded crowd measurements from 2026-10-17, with pose and anchor behavior alternated in the same session, reported approximately **52.8 to 27.1 posed skeletons per frame**, **0.94 to 0.76 ms** for poses, and **1.84 to 1.67 ms** for scene synchronization. These figures describe that combined comparison, not an isolated guaranteed gain for pose scheduling alone.


## 5. Skeleton evaluation and vertex deformation are separate jobs

### 5.1 The CPU evaluates a skeleton, not every deformed vertex

Animation controllers evaluate the avatar's current joint transforms on the CPU and produce a joint palette. Independent skeletons can be evaluated in parallel. Bodies and rigged worn meshes reference the palette of their owning skeleton.

The GPU vertex shader combines the current joint transforms with a mesh's inverse-bind transforms and blends up to four influences for each vertex. Zero-valued secondary influences avoid their additional matrix work. The shader normalizes the weight sum.

This means an animation frame generally updates matrices, not a freshly CPU-skinned copy of every body, clothing, and hair vertex. Skinning is performed by vertex shaders in the passes that need the geometry. It is **not** a claim that a compute pass skins each vertex once and shares a deformed vertex cache across every pass: multiple view and depth passes can repeat vertex deformation.

### 5.2 Mesh geometry and skin bindings are shared

Skin bindings are stored once per mesh asset as inverse-bind matrices plus mappings to palette joints. Draw records reference their binding range. Multiple users of the same mesh do not require separate copies of those immutable bindings merely because their poses differ.

Binding ranges are returned when the last geometry using a mesh is freed and reused by subsequent meshes. Dirty adjacent additions can be combined before uploading. This prevents a long session from accumulating an unreclaimable skin-binding range for every rigged mesh ever encountered.

The base rig, animation assets, geometry entries, and immutable skeleton data are reused rather than rebuilt as per-face resources. Individual owners retain the appearance and pose state that must differ.

### 5.3 Rest-skeleton reconstruction is cached separately from motion

Shape parameters, mesh joint overrides, and attachment membership determine an owner's rest skeleton. These are not recomputed merely because an animation advances.

The application caches the result with an appearance generation and a signature of relevant members and skin metadata. Object changes identify affected skeleton owners; unchanged owners avoid even rehashing their whole membership on the fast path. Newly arriving mesh metadata can require a broader recheck.

A safety refresh remains, but its maximum age is staggered by owner over approximately **two to three seconds**. A crowd that arrived together therefore does not repeatedly rebuild all rest skeletons on the same two-second boundary.

placed_model.

## 6. GPU anchors eliminate repeated CPU placement of worn parts

This is one of the most consequential CPU optimizations for crowded scenes with elaborate outfits.

### 6.1 Share the transform that actually changes

Each skeleton palette includes a world transform for the skeleton root and transforms for its relevant body attachment points. These are called anchors.

A rigid worn part stores its placement **relative to its attachment anchor**. A rigged mesh uses the skeleton root anchor together with the skeleton's joint palette. The GPU composes those transforms when drawing.

As the wearer walks or animates, the CPU updates the shared anchor transforms rather than revisiting every linked prim and rewriting its world matrix in every face record. A complex outfit can have many objects and faces but relatively few independent attachment transforms.

An attachment must still be synchronized when its own state changes, when an intermediate linked transform changes, when it is edited or detached, or when its detail level needs attention. Anchoring removes redundant inherited movement; it does not ignore independent movement or edits.

### 6.2 Visibility bounds follow the same GPU placement

GPU culling transforms rigid attachment spheres through their anchors. For skinned meshes, background geometry preparation computes bounds per influencing joint. Each frame, a compute pass poses these joint boxes and derives current object bounds before face classification.

This avoids CPU skinning solely to find visibility bounds and avoids relying only on a permanent oversized sphere for every animated mesh. Fallback bounds remain available when detailed joint bounds cannot be used.

Some CPU consumers still need approximate current positions: transparent ordering, hover, lights, and impostor planning. Their centers are updated when the relevant anchor changes. Exact transforms are obtained on demand. The optimization does not remove all per-attachment CPU work.

### 6.3 The same design handles independent moving linksets

Ordinary objects with continuous spin or reported motion can receive a mover anchor. Their linked children then follow one shared transform instead of each needing newly uploaded draw records every frame. Mover matrices use separate palette storage, and freed slots have a short quarantine before reuse.

HUD attachments, flexible prims, fading position corrections, and other special movement paths retain CPU handling where the anchor shortcut does not apply.


## 7. Facial animation, body shape, and flexible attachments avoid unnecessary rebuilds

### 7.1 Live body morphs are sparse GPU data

Speech, facial expressions, hand poses, and system-body physics use a shared sparse bank of morph deltas. Per-owner data supplies changing weights. Shaders apply active deltas before skinning, including the corresponding normal changes.

There is no need to regenerate a system body mesh on the CPU for every speech or expression frame. A body part with no active live morphs takes a cheap early path with one activity read. The common relaxed-hand shape is baked into the base shape, and live hand weights describe changes from it.

These deltas are used consistently by relevant surface, shadow, and picking passes. Server-baked appearance textures and baked-texture substitution on worn meshes also let the scene consume composited appearance maps rather than repeatedly composing every clothing layer in the main rendering loop. Static shape construction remains separate work when appearance changes.

### 7.2 Body physics preserves motion across varying frame rates

Body physics uses a **120 Hz fixed simulation step**, samples reference-joint motion between rendered frames, and interpolates the result to display time. Starting at the spring's equilibrium avoids a visible jolt on appearance. Time-based smoothing avoids changing behavior merely because the frame rate changes.

The feature has enable/detail controls, including a screen-size rule for remote avatars at reduced detail. At full detail it can update all eligible avatars, so it should not be presented as universally distance-throttled. This is mainly a motion-quality mechanism; it is not free simulation.

### 7.3 Flexible geometry keeps simulation and remeshing separate

Flexible prims simulate in parallel within draw distance. Their mesh is regenerated and uploaded only if a relevant view can show it: the main camera, water reflection, the active mirror, or the considered sun/moon shadow reach. The current shadow-reach bound is **64 m**.

An offscreen flexible prim retains its last mesh while its simulation advances. When it can become visible again, its mesh is generated from the current simulated state. Existing vertex allocations and face records stay in place between frames instead of being recreated for each sweep.

Recorded source notes describe roughly **640 flexible prims** in each of two busy scenes previously generating **1.3–2.3 MB of vertices per frame**. The culling removes unnecessary sweeps and uploads; it does not stop all offscreen flexible simulation.


## 8. Detail selection and hidden layers reduce geometry demand

Mesh and primitive detail selection depends on object radius, distance, field of view, and the configured detail factor. It selects among authored levels rather than assuming every object always needs its highest-detail geometry. The distance rule includes near-camera behavior and deliberate rounding of thresholds.

For stationary objects, the application computes how far the camera can travel before the current level might change. A priority structure schedules a new detail check only when that conservative travel allowance is exhausted. This avoids repeatedly visiting every static object just to rediscover the same level. Camera jumps and relevant settings changes make the checks due again, with work bounded across frames.

Anchored objects need periodic checks because they can move independently of camera travel. While the camera rests, their check period is time-based, approximately **0.4 seconds**, rather than becoming more expensive simply because the display frame rate rises. A **10-second** safety sweep catches missed changes.

Fully transparent faces and designated invisible textures are omitted from ordinary rendering. Baked-on-mesh visibility handling suppresses system parts replaced by the worn appearance, and visibility masks are refreshed from relevant appearance/attachment changes rather than exhaustively rediscovered every frame. Selection and diagnostic views have their own exceptions.

Important limit: authored avatar mesh levels are often poor. The task list still identifies generated simplification of rigged meshes as future work. Do not attribute current crowd speed to an implemented automatic mesh-simplification pipeline, mesh shaders, meshlets, or a universal per-avatar triangle budget. None of those is established here as the mechanism driving the current avatar path.


## 9. Most opaque drawing is GPU-driven

### 9.1 Persistent tables replace rebuilt CPU command lists

The scene maintains persistent GPU tables for drawable faces and objects. Face entries identify geometry and rendering class. Object entries supply bounds, flags, skeleton ownership, and current avatar rendering state. Only changed ranges are uploaded.

Compute passes test objects/faces for each required view and compact surviving work into bins of indirect draw arguments. The GPU also supplies the counts used to draw those bins. The current engine defines **31 bins**, including main-view classes, shadow views, reflections, probe work, and water-exclusion work.

This avoids a CPU draw call and a CPU visibility-list entry for every ordinary opaque face in every view. Shared geometry buffers and shader-readable draw records let many draws be submitted through multi-draw indirect operations.

The implementation requires the appropriate indirect-count device feature and sufficient table capacity. Otherwise it keeps a CPU list-building fallback. Diagnostic forcing of the CPU path also exists. GPU-driven drawing must therefore be described as the normal supported-device path, not a guarantee on every adapter.

### 9.2 Some lists deliberately remain on the CPU

Transparent faces need controlled ordering. Terrain, water, glow, impostor captures/cards, selection, and debugging also retain CPU list work where appropriate. The CPU uses compact indexes to find these objects instead of walking every large scene record.

A **one-byte classification index per object slot** identifies avatars, special faces, drawn objects, and HUDs. This matters after visiting a large region: a persistent slab can retain a high maximum slot count even after most objects disappear. Reading one compact byte per slot avoids repeatedly loading megabytes of large object records.

Texture-use tracking is normally distributed over **30 frames**, with newly rebuilt faces checked immediately so a newly needed texture is not assigned the wrong first request size or priority.


## 10. Hidden pixels and redundant shadows are removed before expensive work

### 10.1 Two-phase hierarchical depth occlusion

The depth prepass first draws candidates known to have been visible previously. The renderer builds a hierarchy of depth values from that result and tests remaining bounds against it. Newly revealed geometry is admitted to a second depth prepass in the current frame.

This temporal two-phase structure avoids trusting last frame's occlusion so strongly that newly exposed objects visibly pop in a frame late. The GPU path compacts the two prepass lists instead of issuing an entire bin full of disabled draws.

The color pass begins with established depth and rejects covered fragments before expensive shading where the pipeline allows early depth testing. With multisampling, the renderer uses the appropriate multisampled prepass for opaque geometry. Masked edges retain alpha-to-coverage handling instead of blindly sharing an opaque-only depth treatment.

Occlusion is enabled by default in this checkout, but it has a fixed cost and pays most when it rejects heavy hidden geometry. The task notes explicitly record scenes with little gain. There is no established automatic rule that disables it whenever little is hidden.

GPU visibility also does not imply that all hidden avatars stop being posed or that all occluded textures stop downloading. Those additional uses of visibility remain separate issues in the task notes.

### 10.2 Shadow caster culling follows visible receivers

For each sun-shadow cascade, the renderer identifies the portion of the view that will actually sample that cascade and stretches that receiver volume toward the light. Casters outside that volume cannot affect the visible receivers and are rejected.

This is more selective than rendering every caster inside a broad cascade square. Per-pixel cascade selection gives farther cascades a meaningful nearest receiver and permits more accurate exclusion of near casters from irrelevant farther work.

Opaque casters use a depth-only path with no fragment shader. Masked and relevant blended casters retain the alpha test needed to preserve hair, foliage, and other cutouts.

### 10.3 Opaque cascade depth is reused

Where an opaque caster is already represented by a finer cascade, the renderer can omit drawing it again into a coarser cascade. Two small GPU passes transfer the covered depth region through a scratch texture into the coarser map, preserving the nearer depth.

This directly matters for avatars: otherwise a fully rendered avatar can be skinned and rasterized into two or three cascades in the same frame. Reusing depth removes redundant opaque caster draws and their vertex work.

Alpha-tested casters are **not** shared this way. Different shadow-map texel footprints can cut their alpha textures differently, so they are drawn after the merge in each required cascade. Describing every avatar as drawn into only one cascade would therefore be incorrect.

Projector shadows also have an explicit map-count budget. The constructor default is **two maps**, with up to **16** supported subject to settings and device limits. This bounds another source of repeated scene rendering while preserving the intended per-map resolution.


## 11. Transparency is optimized within visible correctness constraints

The renderer separates opaque, masked, blended, and two-sided variants. Fully invisible faces do not pay the ordinary surface-rendering cost. Eligible ordinary objects with sufficiently binary alpha can use masking, which participates more efficiently in depth handling than general blending.

Automatic conversion to masking deliberately excludes rigged faces, HUD attachments, and glowing faces. Hair, soft body layers, and similar content cannot be accelerated by arbitrarily treating their soft alpha as a hard cutout.

Rigged blended faces are grouped by skeleton and object, with a stable attachment/face ordering. Other blended faces are sorted back to front. Face-distance calculation factors the object's transform once; each face then needs only a small amount of work from its precomputed bounds.

The normal layered rigged-alpha path uses a separate copy of scene depth. For each object, it draws the more-covered color, draws softer fringe color, and then writes the fringe depth into that private depth copy. A final pass updates ordinary scene depth only where coverage reaches the half-coverage threshold. This lets later rigged layers respect earlier objects without letting a very faint hair edge incorrectly block water, particles, or glass in the ordinary world pass.

This is a deliberate correctness cost, not an optimization to count as free. The source implements several passes for this path. Repository notes identify layered hair overdraw at high resolution as an unresolved cost. GPU-driven opaque drawing and impostors reduce surrounding work, but they do not make overlapping transparent hair, clothing, and particles inexpensive.


## 12. Lighting, probes, and screen-space effects bound per-pixel cost

### 12.1 Main-view lights use conservative spatial candidate sets

Instead of testing all **64 local lights** at every lit fragment, the main view is divided into **32 columns, 32 rows, and 32 logarithmic depth slices**. Each light contributes a bit to the ranges it reaches. A fragment intersects the sets for its column, row, and slice and evaluates only those candidate lights.

This is a compact set-intersection scheme rather than a stored list for every full 3D cell. It remains conservative: unnecessary candidates may survive, but a light that can contribute must not be lost. Lights retain their original accumulation order, avoiding a numerical or visual change just from reordering them.

Crowds make this useful because face lights and worn lights can fill the frame's light list even though any one surface is affected by only a few of them. Reflection-probe candidates use analogous sets, with capacity for **256 probes**.

Other views, including impostor captures and reflections, retain the full light list. The optimization should not be generalized to every render view.

### 12.2 Probe updates are amortized

Reflection probes are cached cube maps, not six fresh scene views for every probe every frame. The scheduler captures **one cube face per frame**, prioritizing probes by need and distance. The default environment probe has a **two-second** refresh period.

Automatic probe cells start at **16 m** near the camera and become larger with distance, up to **256 m**. This lets a bounded probe pool cover the draw distance without spending most slots on tiny far-away cells. Candidate discovery is spread over several frames rather than charging one frame for its entire periodic scan. Old captures survive until replacements are ready, reducing lighting popping.

Avatars, their attachments, and animated mesh objects are marked out of ordinary reflection-probe captures. This prevents each cached environmental reflection update from repeatedly rendering the crowd. Direct mirrors and other reflection paths are separate and still need their own culling and quality budgets.

### 12.3 Resolution-sensitive effects have cheaper paths

Ground-truth ambient occlusion uses a depth hierarchy, temporal history with rejection, and edge-aware filtering. Low quality and render areas of **3.2 million pixels or more** use half-resolution evaluation, followed by depth-aware upsampling. This saves substantial pixel work while avoiding a plain blur across object boundaries.

Optional temporal upscaling can render the world below display resolution, then reconstruct the output using motion and reactive information. This can reduce both avatar and world shading cost when the GPU is pixel-bound. It is a quality/settings choice; it must not be described as always active. The source implements the upscaler, not an established frame-generation path that invents additional display frames.

GPU motion history retains previous object matrices and joint palettes. The depth pass can therefore produce motion vectors for moving and animated geometry, which helps temporal anti-aliasing and reconstruction follow avatars instead of treating every animated edge as unrelated history. This improves temporal stability and reduces shimmer; it is distinct from increasing raw rendering throughput.


## 13. Texture handling is designed for thousands of unique assets

### 13.1 Paged bindless textures reduce both binding and validation cost

Draw records hold stable integer texture slots. A GPU lookup maps each slot to a page and a layer. Textures with compatible dimensions and mip counts share layers of a texture-array page. The shader selects the page and layer directly.

This avoids binding a separate material texture set for every draw. More subtly, it reduces the number of bound resources the graphics abstraction must track when finalizing command buffers. A busy scene with approximately 9,400 textures can be represented by a few hundred pages rather than thousands of separately bound texture resources.

Ordinary texture relocation or streaming updates rewrite the location table; the binding group changes only when pages are created or dropped. The default table has **262,144 slots**. A slot without resident texels is cheap and points at a shared placeholder. Multiple initially missing textures therefore do not each allocate and upload an individual tiny grey texture.

Page creation is normally requested ahead of pending uploads and performed by a separate worker. Page retirement is also moved away from frame-critical threads. Special direct-created textures can still take synchronous paths; this is not a blanket claim that no resource is ever created during a frame.

### 13.2 Compression saves bandwidth and residency, with a quality gate

Eligible decoded textures are converted on workers to BC7 blocks. For the same texel dimensions, the format uses roughly **one quarter of RGBA8 storage and upload bytes**, reducing VRAM use and sampling/upload bandwidth.

The encoder checks reconstructed blocks against source texels at each mip, with extra protection for alpha endpoints and just-below-opaque values. Textures that fail the acceptance criteria stay uncompressed. Thus this is lossy compression with safeguards, not a guarantee of mathematical identity.

Historical source notes report about **85% of textures passing** and approximately **2.9 times lower aggregate texture memory** on a sampled cache. Difficult normal maps and fine alpha details explain why applying compression indiscriminately would be incorrect.

### 13.3 The disk cache stores work already done

The transcoded cache retains accepted compressed mip chains together with alpha classification, picking masks, and source-image metadata. Returning to a region can read an appropriate cached mip range and upload it directly, avoiding another image decode, mip generation, and compression pass.

A texture that compression would harm records that decision so subsequent decoding does not repeatedly try the same expensive failed conversion. Cache reads validate sizes and format versions. More detailed requests can still require original-image decoding if the cached chain is too coarse.

### 13.4 Progressive requests match current visual need

Texture requests begin with a size chosen from current demand, with a minimum initial request of **16 KiB**. Coarser useful pixels can appear while finer data is still requested. The full-resolution decode waits for complete data, avoiding the known blurry result from attempting the finest decode on an incomplete source.

Demand considers projected size, camera distance, centrality, and texture repeats. Under suitable conditions, nearby central content receives an area boost; fast camera motion or memory pressure removes that boost. Material maps for one face share the sizing decision rather than being loaded at arbitrary unrelated resolutions.

Meshes and textures share a visibility/size-based bulk priority scheme. Urgent work includes animation, appearance-critical assets, the own avatar's worn meshes and bakes, and interface requests. Request ranks can change with the view, and unused requests can be dropped. This improves useful arrival time instead of treating every newly encountered asset equally.


## 14. Mesh downloads and geometry preparation avoid arrival bursts

For meshes first needed at lower detail, the streamer requests only the useful beginning and missing blocks rather than always downloading every detail level and physics representation. Initial requests are **8 KiB** for the two coarsest levels and **32 KiB** for medium detail. A mesh initially needed at highest detail is requested whole to avoid adding a round trip before a nearby object can appear.

Missing nearby byte ranges are joined when appropriate. Header and skin parsing, geometry decoding, tangent construction, picking structures, and per-joint bounds are background work. Assets and geometry are shared by their keys so multiple users do not independently repeat identical preparation.

Geometry lives in large suballocated vertex, skin, and index buffers. A best-fit allocator indexed by free-range size avoids linearly searching thousands of holes after detail-level churn. Adjacent freed ranges coalesce for reuse.

The renderer can grow geometry storage ahead of need, including behind a loading screen using remembered regional peak demand. Buffer growth uses GPU copies to retain existing content. Anticipatory growth is bounded by available memory and can fall back to demand-driven growth. This reduces sudden buffer replacement during normal movement, but it does not make geometry storage unlimited or automatically shrink every arena after leaving a region.

Vertex storage is compact: the common vertex is **24 bytes**, with packed normal information and a tangent encoded into the existing normal stream. Skin data is a separate stream with four byte-sized joint indices and four normalized **16-bit weights**. The latter preserves fine deformations that coarser weights would visibly terrace. Indexed geometry uses 16-bit indices in the shared arena. These choices reduce memory traffic without expanding every static vertex to carry full skeletal data.


## 15. Loading and rebuilding are bounded frame work

### 15.1 Scene synchronization follows changes

The scene tracks moving, dirty, pending, newly arrived, and backlogged objects. It does not perform a complete semantic rebuild of every object every frame.

Cheap current movement remains responsive, while expensive full synchronization is governed by a default **1 ms budget**. New arrivals and backlog receive bounded shares, and deferred work favors relevant nearby content. This is a scheduling target, not a hard real-time ceiling: one individual operation or obligatory state update can exceed it.

Objects waiting for geometry that cannot yet progress are parked out of the normal pending round and reactivated when their geometry changes, with a **two-second** safety retry. Thousands of objects waiting on the same arrival bottleneck no longer impose thousands of identical readiness checks every frame.

### 15.2 A material arrival invalidates only its actual users

Each material records the generation when it changed. An object is rebuilt only when one of its own materials changed after its faces were built. Unrelated material arrivals do not invalidate every object that happens to use any material.

This is especially important in crowds, where each arriving outfit can introduce multiple new materials. A global invalidation would create repeated waves of expensive rebuilding across already loaded avatars.

Recorded live-scene comparison notes report approximately **14.4 to 0.9 rebuilt objects per frame** for targeted invalidation in one alternated session. That result is workload-dependent; a scene with no new materials has little opportunity for this particular saving.

### 15.3 Assets with no work are absent from recurring work lists

Mesh, material, and texture streamers maintain active queues for reads, downloads, retries, decoding, and uploads. An asset already loaded and unchanged should not cost a hash-table visit on every frame merely because it was seen earlier in the session.

When most textures are active during an arrival burst, the implementation can switch to walking the table in its natural order rather than performing a separate lookup for every active queue entry. Periodic maintenance and eviction use bounded queues and expiry tracking.

Unused textures normally wait **45 seconds** before eviction, allowing quick returns without immediately reloading everything. Eviction is limited to **64 textures per frame**, with bounded queue examination. Under pressure, unused textures can expire after **five seconds** instead.


## 16. Uploads avoid copying large assets on the main thread

### 16.1 Workers write the upload memory themselves

Texture decoding and geometry workers write finished data directly into reusable mapped staging chunks. The main thread records copies from those chunks instead of copying every finished image or mesh into a newly allocated staging buffer.

The usual pool is **eight 32 MiB chunks**, with a smaller **four 16 MiB** configuration for constrained devices. A worker can briefly wait for staging space, up to approximately **100 ms**, then keep data for a bounded fallback upload if necessary. Worker jobs do not allocate new GPU chunks themselves, avoiding allocator-lock contention with frame resource creation.

Chunk ownership explicitly tracks active writers, pending copies, submission, and asynchronous remapping. A chunk is not recycled while a worker is writing it or a GPU copy still needs it.

### 16.2 Small recurring writes are batched too

Bone palettes, flexible vertices, object tables, and draw-record updates can generate thousands of small writes in one frame. The renderer records them in a per-frame write journal and usually replays buffer writes through a single reusable mapped staging buffer, merging consecutive compatible runs.

This avoids a separate buffer allocation, map, and retirement for every small queue write. Up to **three** staging buffers rotate, with a **32 MiB per-buffer staging ceiling**; exceptional cases retain a direct-write fallback. Copy order relative to arena growth and rendering is preserved.

Dirty-range tracking also limits what is sent. The fact that one object changed does not require retransmitting a whole broad chunk containing unrelated objects when the exact-range path applies.

Historical source notes identify **3,900 small buffer writes in one slow frame** and resource-replay costs of **3.3–5.5 ms** before batching in busy scenes. Those observations explain why reducing API and allocation count matters even when the byte total is modest.

## 17. Memory policy prevents texture thrashing and driver stalls

The texture target is constrained by the driver-reported budget and by memory already used for geometry, render targets, and other resources. The implementation does not assume nominal board capacity is entirely available to textures.

Important stabilizers include:

- Budget drops are honored promptly; rises are held until sustained, using the lowest observed budget over approximately **60 seconds**.
- Pressure rises only after sustained excess, and relief requires headroom and an estimate that the next finer state fits.
- A reduction step targets enough savings to bring pages below roughly **85% of the target**, preferring large reductions rather than lowering every eligible texture indiscriminately.
- Reduction proceeds in partial-level steps across the texture population. Offscreen content loses detail before important visible content.
- Resident textures are shrunk by copying already present mip levels on the GPU, avoiding another source decode.
- Avatar bakes, HUDs, interface content, sculpt maps, and terrain have protected handling; the ordinary pressure rule does not indiscriminately reduce every texture.
- Page compaction and retirement let empty pages disappear. On Vulkan, dedicated allocation per texture page helps return its memory to the driver when the page is destroyed rather than leaving it trapped in a larger allocation block.
- Allocation failures keep usable existing buffers, retry later with backoff, and avoid repeatedly installing invalid resources or flooding logs.

Instances on the same GPU exchange memory-use and foreground state through a background service. Texture budgets can account for other active instances. Background instances can reduce their rendering rate, relinquish texture detail after the configured delay, or pause ordinary 3D drawing under the relevant pressure policy. On Windows, an additional worker reads whole-adapter memory use so oversubscription by other programs can also influence the target. That adapter-wide reading is platform-specific.

These mechanisms protect smoothness by reducing paging, repeated redecoding, and unload/reload oscillation. They cannot guarantee good frame times when geometry and render targets alone consume most of a small GPU's memory. The task notes retain this as a real limitation.

## 18. Parallelism is arranged to reduce scheduling stalls

### 18.1 Main and render threads overlap adjacent frames

The main thread owns input, world updates, scene state, and interface composition. A dedicated render thread owns queue writes/submission, command encoding, presentation, and interface tessellation. While frame N is encoded and rendered, the main thread can prepare frame N+1.

A self-contained frame packet carries ordered writes, buffer handles, draw lists, camera data, interface shapes, and readback requests. The render thread is the sole queue owner, preventing next-frame updates from accidentally leaking into an earlier submission.

The handoff is a rendezvous: no unbounded packet queue accumulates behind a slow render thread. Two persistent helper threads finish independent command encoders in parallel. Keeping those helpers alive avoids thread creation and cold-start scheduling costs every frame.

### 18.2 The frame thread helps with its own parallel loops

For placement planning, pose evaluation, flexible simulation, and list work, the calling thread starts processing chunks immediately. A limited number of pool workers join as they become available. A short loop requests no helper, and a worker that wakes after the work is gone is not waited for merely because it was requested.

This avoids sending a tiny frame-critical loop to a busy pool and sleeping until workers wake. Work granularity follows expected cost: skeletons and flexible prims use small chunks; cheap object work uses larger chunks. Up to **seven helpers** can participate.

There is still a join for a helper already holding borrowed work. The design reduces scheduling risk; it does not eliminate synchronization or promise that a preempted helper can never delay a frame.

### 18.3 Background work does not get equal scheduling priority

Decoding and mesh-building jobs run at lower priority. Frame-critical threads and participating helpers can receive a modest priority increase. This prevents an arrival burst using many cores from treating texture throughput as more urgent than input and the next visible frame.

The process normally uses **mimalloc version 2**, reducing contention between frame allocations and background workers. Large discarded CPU payloads are released by a separate disposal thread. Hot per-frame maps use a fast hash implementation rather than the default general-purpose hasher. Reused vectors, pools, and pre-touched scene storage reduce allocation and first-touch costs.

These are smaller than replacing thousands of redundant object updates, but they address tail latency under load, when a nominally short allocation or worker wake can otherwise turn into several milliseconds.

## 19. Smoothness also comes from input timing and motion reconstruction

### 19.1 Adaptive pacing avoids stale input without always serializing stages

When the GPU is the sole bottleneck, running the CPU far ahead can leave a mouse movement waiting behind several frames. The pacing path waits for the appropriate previous GPU-frame completion **before reading the next frame's input**, keeping the GPU queue shallow.

The device wait is performed by the render thread, which notifies the main thread. The main thread does not poll the device while holding locks that would block submission or presentation. The input-side wait is bounded, approximately **100 ms**.

The wait is enabled only when smoothed GPU time covers the combined main-thread and render-thread work, with hysteresis. When the CPU stages are competitive with GPU time, waiting would reduce overlap and can substantially lower throughput; the gate disables it. Without usable GPU timestamps, the adaptive gate does not assume this condition is satisfied.

This is a latency optimization. Historical comparison notes include similar frame rates with substantially shorter input-to-presentation delay. It should not be summarized as an unconditional extra wait or as a method of increasing the physical GPU's rendering speed.

### 19.2 Movement is reconstructed between simulator updates

Remote objects and avatars are displayed using reported velocity, acceleration, and angular motion between network reports. Their clock follows the region's time dilation. Position corrections fade with a **60 ms half-life** where the motion rules require smoothing, and seated/vehicle motion has corresponding damped behavior.

This decouples visible movement from network packet cadence. Frame-rate-independent evaluation and damping avoid introducing different motion simply because display frequency changes. Prolonged circuit silence slows motion after **two seconds** and stops it at **three seconds**, rather than extrapolating indefinitely through a missing connection.

This does not improve simulator performance or network latency. It makes the local presentation continuous using available motion information.

### 19.3 Picking avoids an avoidable frame stall

Normal click picking requests depth and the drawn surface asynchronously. A small **2 × 2** render target uses the same posed geometry and relevant visibility rules to identify the clicked surface. Results return in a later frame, ordinarily one to three frames, rather than forcing the interface thread to wait for all GPU work.

The picking pass is requested for a click, not redrawn and reallocated for every ordinary frame. Hover uses a compact spatial index, cached queries with a short maximum lifetime, and worker-prepared triangle acceleration/masks where applicable. This prevents the mouse merely resting over a crowd from triggering a full expensive surface search every display frame.

## 20. Supporting systems keep non-render work from consuming crowd savings

Several mechanisms matter because a fast GPU cannot hide a main-thread stall elsewhere:

- Avatar lists, floating-text lists, particle changes, and sound changes are maintained from object events. Consumers avoid rediscovering these categories by scanning every world object each frame.
- Nameplates reduce detail with distance and rank. The richer mode normally limits full plates to **eight**, uses full/compact distance thresholds of **15 m and 40 m**, and retains cached painting/layout data. Other tag styles have their own behavior; those thresholds are not a universal rendering rule for every name style.
- Large interface lists and chat histories use visible-row/message rendering and cached layout measurements. Interface shapes are tessellated on the render thread.
- Inventory-dependent restriction checks cache resolved folder rules and rerun watchdog work when relevant generations or worn state change, avoiding repeated whole-inventory traversal for each attachment each frame.
- Distant regions left connected after a teleport can be parked when beyond **1,024 m** of reach. Their state and intervening events remain available for restoration, but their objects leave the active scene and stop consuming ordinary render/stream/particle upkeep.
- Pipeline compilation results can be cached on disk on the supported Vulkan path, keyed and validated for the GPU, driver, and build. This chiefly improves startup and repeated pipeline creation, rather than steady-state avatar cost.
- Settings persistence, cache-file writing, transcript work, and heavy asset processing use background services. Bounded queues avoid trading frame smoothness for unlimited memory growth.
- Surface presentation uses an explicit mode-selection rule. GPU selection favors appropriate hardware and can fall back if opening an adapter fails. These influence throughput and latency but do not substitute for the scene optimizations above.

## 21. What another implementation must preserve

The transferable design is a set of dependencies. Implementing just one half often retains most of the old cost or introduces visible defects.

| Mechanism | Required relationship | Failure if omitted |
|---|---|---|
| Avatar impostors | Full-render selection must govern the body and all its attachments. | Clothing and hair continue consuming full draws behind the card. |
| Picture-driven posing | Pose eligibility must follow capture eligibility, while animation state still advances. | CPU animation cost remains high, or refreshed pictures show incorrect animation phases. |
| GPU attachment anchors | Draw placement, visibility bounds, and remaining CPU position consumers must agree. | Incorrect culling, transparent ordering, hover, or attachment placement. |
| Cached skeleton bases | Appearance, topology, late mesh metadata, and joint overrides must invalidate the right owner. | Stale proportions or needless whole-crowd skeleton rebuilds. |
| GPU-generated draw lists | Persistent face/object tables must carry the same rendering state as the CPU fallback. | Hidden attachments reappear or special views render different content. |
| Temporal occlusion | Newly revealed content must be reconsidered in the current frame. | Visibility popping or missing geometry after camera movement. |
| Cascade reuse | Only compatible opaque depth is shared; alpha-tested work keeps its required sampling. | Incorrect hair/foliage shadows or apparent holes. |
| Paged textures | Stable slots must survive relocation and page retirement; old in-flight frames keep needed resources alive. | Wrong textures, flashes, or use of retired resources. |
| Worker staging | Memory ownership must cover worker writes, GPU copies, submission, and remapping. | Corrupt uploads or synchronization stalls. |
| Time budgets | Deferred work must retain progress, priority, and reactivation paths. | Permanent grey assets, starvation, or many-frame stalls. |
| Memory pressure | Unloading and reloading thresholds must differ, and finer restoration must actually fit. | Repeated blur/sharpen cycles and decode/upload thrashing. |
| Render overlap | Frame packets must be internally consistent and queue depth must remain bounded. | Mixed-frame state or growing input latency. |
| Adaptive pacing | Enable low-latency waiting only when it does not unnecessarily destroy stage overlap. | Lower frame rate on CPU-heavy scenes. |
| Flexible remesh culling | Simulation must remain current and visibility must consider secondary views and relevant shadows. | Frozen motion on reappearance or missing reflected/shadow geometry. |

## 22. Limits and conclusions supported by the evidence

The code supports a strong architectural explanation for high crowd throughput: **bounded full-avatar rendering, temporally cached distant avatars, shared CPU poses with GPU deformation and placement, GPU-generated visibility/draw work, reused shadow depth, and incremental background-fed scene maintenance**.

Smoothness has additional causes: **bounded arrival work, reduced allocation contention, worker-written upload memory, batched recurring writes, memory hysteresis, continuous local motion, and input-aware pacing**. A high average frame rate alone would not explain these.

The investigation does not establish that every busy scene is fast. Important remaining costs and limits include:

- Fully rendered avatars can still have millions of triangles, especially with poor authored detail levels.
- Rigged transparent hair and clothing can require several passes and substantial overlapping pixel shading.
- Shadow rendering repeats skinning for the views and alpha-tested casters that still need it.
- Impostor capture capacity and atlas memory are finite, and the cards intentionally reduce temporal and viewpoint fidelity.
- Some full avatars outside the main frustum can still require pose/synchronization work; complete visibility-driven suppression of that work is not established.
- GPU occlusion is not automatically a CPU-animation or texture-fetch suppression signal.
- Geometry and render targets can exhaust memory even after textures are reduced.
- CPU-list fallbacks, different APIs/drivers, multiple applications sharing a GPU, and saved quality settings change the performance balance.
- Historical task entries include pending validation and superseded implementation notes. Active sources are the authority for present behavior; old timing figures should not be combined as though they measured one final build.

To measure these mechanisms in a comparable application, hold camera, resolution, draw distance, avatar count, full-avatar limit, detail factor, shadows, transparent content, anti-aliasing, cache state, and foreground load constant. Separate main-thread work, render-thread work, GPU pass times, frame-time percentiles, input latency, memory residency, and asset-arrival latency. Compare one mechanism at a time or explicitly label combined changes. The existing source contains comparison and verification modes for many of these paths, which makes causal testing possible without treating visual quality reductions as unexplained speedups.


