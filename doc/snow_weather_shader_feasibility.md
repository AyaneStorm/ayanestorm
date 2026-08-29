# Snow Weather Shader Feasibility

Author: chanayane@firestorm

## Conclusion

An optional, viewer-local **Snowing** effect is feasible only if its shelter
behaviour is part of the design. The useful part of
`snow.shader` is the procedural `snowing()` function. The file is Shadertoy
source rather than a shader that can be loaded by the viewer unchanged, so it
must be ported to AyaneStorm's GLSL conventions and render pipeline.

The visual implementation should be described as a viewer-local weather effect,
not simulator weather or a physically located particle system. It can look
convincing while the camera moves, but it will not affect the region, other
residents, lighting, terrain, or object surfaces.

## Assessment of `snow.shader`

### Reusable

- The 66-layer procedural flake generator in `snowing()`.
- Its depth, width, and speed concepts, after replacing mouse-driven values
  with explicit uniforms/settings.
- Its time animation, mapped from `iTime` to `gFrameTimeSeconds`.

### Must be replaced or removed

- `mainImage()` is a Shadertoy entry point; the viewer requires its normal
  fragment-shader interface and output declarations.
- `iResolution`, `iTime`, and `iMouse` must become AyaneStorm uniforms.
- Mouse position currently controls snow depth/width/speed. This is unsuitable
  for viewer operation and should become panel controls.
- `samplerXX iChannel0..3` is placeholder documentation, not valid GLSL.
- `background()`, simplex noise/FBM, and the disabled Mystery Mountains code
  create a replacement scene and are inappropriate for an overlay on the
  existing Second Life view.
- The current final addition also adds alpha (`fragColor += ...`), which is not
  a correct compositing contract for this viewer use.

The source file contains attribution links but no explicit licence statement.
Before distributing a derived shader, confirm the licence/permission for the
two attributed Shadertoy sources or replace the algorithm with an independently
implemented procedural snow pattern.

## Recommended Architecture

Create AyaneStorm-owned files rather than enlarging an upstream module:

- `asweather.h` and `asweather.cpp`: shader lifecycle, settings validation,
  render call, and optional Weather-panel callbacks.
- `app_settings/shaders/class1/deferred/asweatherV.glsl` and
  `asweatherSnowF.glsl`: full-screen triangle and procedural snow overlay.
- `panel_as_weather.xml`: outer Weather tab container.
- `panel_as_weather_snow.xml`: first inner tab, **Snow**, with the master
  **Snowing** switch.
- A small standalone weather floater is recommended so the preferences button
  has an unambiguous target; its content can reuse `panel_as_weather.xml`.

Follow the established `asaurora`, `aslensflare`, and `asvignette` patterns for
registration in `llviewershadermgr.cpp`, unload/reload handling, settings, and a
small render API. Add all new source and shader files to
`indra/newview/CMakeLists.txt` and the viewer shader file lists as required.

The minimal shared-pipeline integration is one ownership-tagged call from
`LLPipeline::renderFinalize()` in `pipeline.cpp`, after the completed 3D image
is presented and before snapshot guides and UI. This yields snow in the world
view and snapshots without drawing over viewer controls. Exact ordering against
lens flare, isolate background, and vignette should be decided explicitly;
normally snow should be after lens flare and vignette, while isolate mode should
disable weather.

## User Interface Structure

In `floater_as_environment_effects.xml`, add a top-level **Weather** tab whose
panel is `panel_as_weather.xml`. That panel contains its own tab container; its
first tab loads `panel_as_weather_snow.xml` and is labelled **Snow**.

The Weather panel is an extensible container, not a Snow-specific panel. Its
inner tab container must be laid out and named generically so later **Rain**,
**Wind**, fog, or other weather tabs can be added without restructuring the
floater. Weather-wide controls, if introduced later, belong outside the effect
tabs; effect-specific controls stay within their own panel files.

The Snow tab initially needs:

- **Snowing** master checkbox, default off.
- Intensity/density.
- Fall speed.
- Flake size or depth spread.
- Wind/drift.
- Optional quality selector to reduce the current 66-layer loop.

In `panel_preferences_ayanestorm.xml`, add a **Weather settings** button to the
Rendering tab. It can open a dedicated `as_weather` floater that reuses the same
Weather panel. If no standalone floater is desired, a custom callback must open
`as_environment_effects` and select both the outer Weather tab and inner Snow
tab; a dedicated floater is simpler and consistent with the existing effect
buttons.

## Settings and Behaviour

Add persistent settings to the appropriate AyaneStorm settings file, with safe
defaults. The implemented names are `ASWeatherSnowEnabled`,
`ASWeatherSnowIntensity`, `ASWeatherSnowSpeed`, `ASWeatherSnowSize`,
`ASWeatherSnowLandedHold`, `ASWeatherSnowLandedFade`, and
`ASWeatherSnowShape`, `ASWeatherSnowDistance`, and
`ASWeatherSnowDistanceFalloff`. V1 wind is a deliberately slow fixed drift; a future
Wind tab will own configurable wind.

The implemented Snow appearance is tinted and dimmed from the active EEP
ambient plus the current sun/moon light. It is therefore not fixed white:
night snow is intentionally darker and cooler.

Use a common `ASWeather` settings/module prefix, followed by an effect namespace
such as `Snow`, `Rain`, or `Wind`. Do not make the Weather container depend on
Snow-specific controls or callbacks.

## Extensible Weather Architecture

The new weather implementation should separate orchestration from individual
effects:

- `asweather.h/.cpp` owns shared weather lifecycle and dispatch: shader
  registration, reload/unload, render-stage entry points, shared capture
  exclusions, and resources that multiple precipitation effects can reuse.
- Snow-specific behaviour resides in a separate component such as
  `asweathersnow.h/.cpp` with its own shaders and settings.
- Each future effect receives a separate panel XML file and implementation
  component. Adding **Rain** or **Wind** should require adding a tab and
  registering that component, not modifying Snow code.
- Shared world-space data such as camera-relative weather bounds, shelter maps,
  time, and wind vectors should be produced once by the Weather coordinator and
  supplied to active effects.
- Effects should have independent enable switches. A future optional global
  Weather master switch may coordinate them, but the initial **Snowing** switch
  remains Snow's master control.

Rain can reuse shelter rejection and much of the precipitation-volume
infrastructure while using its own streak rendering and splash behaviour. Wind
may have no direct geometry and instead provide a shared vector consumed by
Snow, Rain, vegetation, or later effects; its UI and implementation should
therefore remain independent from precipitation rendering.

The render function should no-op when disabled, the shader is incomplete, a
reflection/cube capture is active, or background-isolate mode is active.
Consider disabling it for impostor and other off-screen render passes if the
chosen hook can be reached by them.

## Limitations and Risks

- A full-screen shader cannot determine shelter from the current camera depth
  buffer. A roof may be above or behind the camera and therefore absent from
  that buffer. Consequently, an unrestricted screen-space overlay is rejected:
  snowing indoors is a release blocker.
- Pure screen-space motion can appear attached to the camera. Incorporating
  camera orientation and a stable world-space seed improves the effect without
  requiring actual particle geometry.
- Sixty-six procedural layers per pixel may be expensive at high resolutions.
  Quality tiers and early rejection are advisable; the default should use
  substantially fewer layers until GPU timings justify more.
- Post-tonemap white flakes will not inherit scene exposure, fog, or local
  lighting. Rendering before tonemapping is more integrated visually but risks
  bloom and complicates the post-processing chain. A post-tonemap first version
  is lower-risk and matches the desired optional overlay.
- Depth-based occlusion can improve the look but cannot by itself solve indoor
  weather.

## Indoor and Shelter Requirement

**Selected requirement:** snow must remain visible in outdoor space through
windows and open doors while the camera is indoors. Whole-view shelter
suppression therefore does not meet the product requirement and must not be
used as the production implementation.

There are two materially different acceptable behaviours:

### Whole-view shelter suppression

Cast one or several upward viewer-side rays from the camera and optionally the
avatar. When nearby scene geometry blocks the sky, fade the snow overlay out;
fade it back in after the camera returns outdoors. This is compatible with the
procedural full-screen shader and is the smallest practical implementation.

This prevents flakes appearing around the camera indoors, but it also removes
snow from outdoor scenery seen through a window or open doorway. Transparent,
phantom, alpha-masked, very thin, or not-yet-loaded roofs require explicit
filtering rules and may produce false results. Multiple rays and hysteresis
reduce flicker near eaves without changing this fundamental limitation.

### Spatially correct precipitation

To preserve snow outdoors while the camera is indoors, flakes must have
world-space positions and be clipped or rejected against shelter geometry.
Suitable approaches are a camera-centred GPU particle field with collision or
visibility tests, or a precipitation volume using a dedicated top-down shelter
map. The current `snow.shader` is then useful mainly as visual inspiration; its
screen-space layered algorithm is not the correct foundation.

A top-down shelter map is the more scalable GPU design: render nearby occluding
geometry into a height/depth map from above, generate flakes in world space,
and discard a flake when it is below the stored roof height at its horizontal
position. This can keep snow visible outside windows while excluding covered
space. It adds a render target, an occluder pass, camera-relative volume logic,
and careful handling of bridges, overhangs, transparent surfaces, parcel/region
boundaries, and unloaded geometry. It is substantially more work and risk than
the full-screen overlay.

The recommended production design is therefore:

- Maintain a camera-centred, world-space precipitation volume whose flake
  positions are stable under camera translation and rotation.
- Render nearby shelter geometry to a top-down occlusion/height target. Use
  opaque and alpha-masked surfaces; define an explicit policy for blended
  surfaces so ordinary windows do not act as roofs.
- Reject each generated flake below the shelter height at its world XY
  position. Flakes outside the building continue rendering and remain visible
  through windows and doors.
- Depth-test the surviving flakes against the normal scene depth so walls and
  opaque objects occlude them correctly from the camera.
- Keep precipitation out of reflection probes, cube snapshots, impostors, HUD
  rendering, and the self-lighting isolate mode unless a later requirement says
  otherwise.
- Rebuild or scroll the shelter target as the camera moves, with hysteresis at
  its edges to prevent popping.

This architecture should be implemented in a new `asweather` module. The
pipeline will need more than the previously proposed single post-processing
call: a shelter-map pass and a world-space precipitation draw must be placed at
appropriate scene-render stages. Any changes to shared/upstream files remain
small ownership-tagged dispatch points, with the substantial logic contained in
the new module.

The Weather panel should expose no user-facing workaround for indoor snow;
whichever shelter policy is selected must be automatic. A debug visualization
for the shelter test/map would be valuable during development but should remain
an advanced diagnostic.

## Feasibility Decision

Proceeding remains technically reasonable. The selected requirement mandates
spatially correct precipitation, which is a larger rendering feature and must
use a world-space precipitation module rather than merely porting
`snow.shader`. The original shader can supply aesthetic ideas, but it is not the
implementation foundation. Runtime GPU/visual testing by the user will be
required. No build was attempted during this research.

## Implemented Direction

The selected implementation uses new `asweather` and `asweathersnow` modules,
two top-down depth maps (all blockers versus retaining opaque/terrain
surfaces), CPU particle state, and native `LLVertexBuffer` camera-facing
billboards. Shelter depth is read back at a throttled cadence for CPU collision
and support validation. Landed flakes are limited to surfaces within 50
degrees of horizontal, exclude glass and water, hold for a configurable default
of five seconds, then fade for a separately configurable default of two
seconds.

## Black Framebuffer Diagnosis (2026-08-28)

The first runtime implementation generated both shelter maps from inside the
late post-transparency hook. This was moved earlier because
`LLPipeline::renderShadow()` is a full pipeline pass with broader GL and
draw-pool state than a leaf particle renderer. Runtime stage logging later
showed both maps and the first transform-feedback update completing before the
frame became black.

Shelter capture is now a separate `ASWeather::prepare()` phase at the start of
deferred lighting, after the geometry target has been flushed. Normal deferred
lighting then establishes the final scene state. The late hook only simulates
and draws snow into the composited scene target. The remaining state fault was
the raw Snow path leaving program zero active instead of restoring the shader
recorded by `LLGLSLShader` at the Weather dispatch point. Snow now restores
that program and cache locally around transform feedback and particle drawing.
Further tracing found that `LLPipeline::renderShadow()` also installs its cull
result as the pipeline-global current result. Weather now clears each private
shelter result before reuse and restores the scene result after every capture;
otherwise later scene passes consume Weather's landing-only draw list.

The viewer has no other geometry-shader particle renderer against which the
initial point-to-quad Snow path could be validated. Because every black frame
began after its first raw point draw, billboard rendering now uses an ordinary
instanced triangle strip: four vertex-shader-expanded corners per particle,
with particle attributes advancing once per instance. This removes the
geometry stage while preserving transform-feedback simulation and the same
fragment appearance.

The instanced replacement still produced next-frame `GL_INVALID_OPERATION`
and then an unhandled viewer/driver crash, with no minidump or usable stack.
The raw integration was therefore removed completely. The current renderer has
no transform-feedback program, geometry shader, instancing, raw VAO, raw Snow
buffer, or manual shader-cache manipulation; CPU state is submitted through
the viewer's normal `LLVertexBuffer` contract.

## Shelter Capture Failure and Replacement (2026-08-29)

Visual Studio caught the remaining crash in `LLRender::multMatrix()` while the
ordinary glow pool consumed an invalid `LLDrawInfo::mModelMatrix`. The log also
reported null `mVertexBuffer` entries. A short 30 FPS recording showed windows,
trees, and other detailed geometry jumping at the shelter refresh interval of
0.20 seconds. Together these establish that nested `LLPipeline::renderShadow()`
changes or rebuilds scene draw/cull/LOD data; restoring its cull-result pointer
does not make it safe inside the active scene frame.

The top-down render capture and depth readback have therefore been removed.
Shelter is now sampled without rendering through bounded vertical world
intersection queries. Each particle caches blocker height, surface normal,
glass/water retention classification, and support identity. Queries are spread
over frames, and unsampled flakes remain hidden during the short cache warm-up.
This path does not change render targets, cameras, draw pools, cull results, or
scene LOD state. Foliage, grass, avatars, attachments, and particles are skipped;
glass blocks falling snow but cannot retain it; retaining normals must remain
within 50 degrees of world-up.

Snow uses a configurable 8–128 metre radius (32 metres by default). Particle
reserve and shelter-query throughput scale with radius squared from the default
48,000 particles and 1,536 queries per frame, preserving density and cache
warm-up time as area changes. Shape changes only the procedural flake profile;
Intensity selects a proportional active
subset, so it alone controls flakes per cubic metre and lower intensity avoids
the corresponding simulation, raycast, and geometry work. Compared with the
original 6,000-particle, 128-metre-wide Medium volume, maximum horizontal
density is thirty-two times greater.

The initial deterministic polar spawn reused one linear floating-point key for
radius, angle, height, velocity, and size. Sequential raycast-cache validation
made that correlation visible as curved precipitation bands. Spawn attributes
now use separate integer-avalanche samples, while cache traversal uses a stride
coprime with every quality particle count. This produces uniform coverage and
an evenly distributed cache warm-up instead of strips.

Exact OIT stores its resolved scene glow mask in the screen target alpha
channel. Ordinary alpha blending made Snow coverage alter that mask, producing
strong bloom around flakes only in Exact OIT. Weather now writes blended RGB
while preserving destination alpha, because Snow is lit but non-emissive.

The initial motion update also forced every flake to the identical horizontal
drift vector every frame. At high density this made parallel projected paths
look like repeated flakes falling through fixed lanes. Each recycled flake now
has small stable drift variation plus an independently phased low-amplitude
meander, while the shared Weather vector remains the prevailing direction.
Additionally, after approximately every two metres of descent, each flake
independently selects a slightly different target drift and steers toward it
gradually. This breaks short repeated landing trajectories without producing
visible direction snaps or changing the prevailing Weather direction.

Vertical shelter queries alone allowed a drifting flake to cross a side wall
between cache refreshes. Each falling particle now sweeps the segment from its
last sampled position to its current position when refreshed. Glass, walls,
and steep geometry recycle the particle; near-horizontal eligible geometry is
left to the landing path. Three quarters of the bounded query budget is biased
to particles within 12 metres of the camera so visible indoor crossings are
resolved promptly, while the remaining budget maintains the full volume.
When a vertical probe detects shelter above the camera, falling flakes within
sixteen metres additionally receive a short swept test every frame after
simulation. A current-frame wall or glass crossing is therefore recycled
before its billboard is submitted, without charging that cost while outdoors.

Simulator terrain required a separate fallback: `LLVOSurfacePatch` computes its
intersection step from horizontal ray length and therefore cannot reliably
intersect Weather's perfectly vertical shelter ray. ASWeather now queries the
owning region's land-height field directly and derives a finite-difference
terrain normal for the same 50-degree retention rule. Terrain above water can
retain flakes; the existing water-height removal still prevents retention on
water or submerged land.

Initial allocation formerly overwrote safely recycled heights with a uniform
range from 16 metres below to 32 metres above the camera. That directly seeded
flakes inside rooms. Initial flakes now retain the normal above-camera spawn
height. In addition, the bounded collision refresh checks upward for an
overhead ceiling before its top-down landing query; this supports mesh ceilings
whose upper faces are not returned reliably, without adding a per-particle
every-frame query.

After the missing Volume-partition input was corrected, the initial vertical
distribution was restored. Unsampled flakes are still hidden and building
collisions now participate reliably, so indoor candidates are retired before
drawing while outdoor snow no longer waits to fall 20 metres into view.
At the default 32-metre radius, the shelter-query budget is 1,536 per frame.
Both particle count and query budget scale with horizontal area when Distance
changes, so coverage no longer changes flakes per square metre.

Distance falloff is enabled by default. The configured Distance remains the
full-density radius; an additional eight-metre band uses a smooth density curve
down to zero. Resource scaling uses the outer radius, preserving density inside
the configured distance instead of redistributing its particles into the fade.

Thin mesh walls may expose only one triangle winding to segment intersection.
Lateral flake sweeps now test both travel directions, so either face orientation
blocks a crossing while eligible near-horizontal hits remain landing events.

Further indoor testing showed terrain-valid flakes throughout a room even with
lateral and overhead checks. `LLPipeline::lineSegmentIntersectInWorld()` gates
each spatial partition through the current render-type mask. Weather dispatches
after post-OIT scene passes, where Volume is not guaranteed enabled, so ordinary
building geometry could be absent from every query while the direct terrain
fallback still validated flakes against land beneath the house. Weather now
saves the mask, enables Volume and Terrain for its read-only intersections, and
restores the exact prior mask afterward. This performs no render, recull, LOD,
or draw-info operation.

Because Intensity alone controls volumetric density, the former Quality control
is now Shape: Soft uses a round profile, Hexagonal uses the six-arm profile, and
Branched adds crystalline branches while preserving sixfold symmetry. Distance
separately controls precipitation radius. Collision correctness is independent
of Shape.
The first High secondary arms were sub-pixel at ordinary viewing distances and
looked identical to Medium. They are now thicker and extend nearly to the flake
edge, yielding a readable twelve-arm High silhouette without changing density,
billboard size, brightness, or glow contribution.

Runtime close-ups showed that twelvefold High silhouette looked gear-like,
while Medium's sixfold/hexagonal profile read as natural snow. High now retains
sixfold symmetry and adds paired side branches along the six primary arms
instead of introducing six rotated primary arms.

Weather initially called `LLViewerObject::isImageAlphaBlended()` for every
collision sample. That helper warns for texture formats other than raw RGB(A)
or alpha, producing thousands of log lines for compressed/modern textures.
Glass classification now uses TE alpha, legacy/PBR material alpha mode, and the
already-resolved rendered face alpha pool; it no longer inspects texture GL
formats or emits those warnings.

## Snow Performance Optimizations

Flakes beyond 16 metres use the Soft fragment path regardless of selected
Shape; camera-relative distance is evaluated every frame, so approaching a
flake restores Hexagonal or Branched detail immediately. The fragment shader
avoids arm and polar/trigonometric detail work for distant flakes.

CPU billboard generation rejects particles outside the exact camera frustum
before writing six vertices. Intensity membership uses the existing uniform
particle seed instead of recomputing a float hash in simulation, collision, and
geometry loops. The superseded continuous sine meander was removed because the
new gradual direction target every roughly two metres already supplies unique
motion; this removes one `sin()` per active falling particle per frame and two
floats per particle. Intensity zero releases particle and vertex-buffer memory.

The native six-vertex billboard layout remains unchanged. More aggressive
memory reductions would require returning to instancing/raw buffers or changing
landed/support state representation, both of which carry reliability or visual
correctness risk disproportionate to the saving.

## Large-Distance Random Distribution

At large Distance settings, long landed hold times revealed rows of flakes from
recycled particle cohorts. Particle random streams now start from an independently
avalanched 32-bit state and advance that full state on every recycle. Intensity
selection remains a separate stable low-discrepancy value, and the correction adds
no particle memory.
