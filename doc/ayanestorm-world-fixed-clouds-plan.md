# AyaneStorm world-fixed clouds (Exact volumetric) — Plan

## Context

SL draws its clouds on a dome attached to the camera: moving 1 km shows the
same clouds, the same cloud in front of the sun. Exact mode's cloud shadow
map must match the drawn clouds, so its anchor slides after the camera
(re-centres after 100 m): shadows follow the user, a hill 1 km away lights
up together with the user, and rays cannot be anchored in the world.

Fix: offset SL's cloud UVs by the camera's horizontal motion so the drawn
clouds are fixed in the world (correct parallax), and give the shadow bake
the same offset. Drawn clouds, cloud shadows and cloud rays then match
exactly, anywhere, with no sliding anchor. Colours, density and every
windlight parameter are untouched (additive-only rule is about colours;
positions are fair game).

User decisions:
- Active only while Exact volumetric is on (otherwise vanilla SL clouds;
  toggling Exact restarts the clouds at SL's camera-attached position).
- Cloud layer height becomes a setting (it sets the clouds' physical size
  and parallax speed).

Pending unbuilt changes ship in the same build: cloud map bound with
mipmaps (`bindManual(..., true)`), cloud-ray excess over the local mean,
debug 7 = excess share.

## Maths (verified against the shaders)

`cloudsV.glsl`: uv1 = (vec2(-texcoord0.x, texcoord0.y) - 0.5) / cloud_scale
+ 0.5, uv2 = uv1 + light term, uv3 = 16 uv1, uv4 = 16 uv2. Dome UV
(`LLVOWLSky::buildStripsBuffer()`, mirrored in `asVolumetricCloudShadowF.glsl`):
texcoord0 = ((-dir.z + 1) / 2, (-dir.x + 1) / 2), dome x = north, z = east.
On the physical sphere (radius R = H / (1 - dome_offset), centre below the
viewer), dir.z = east / R, dir.x = north / R of the hit point relative to
the viewer, so:

    d uv1 = (d east, -d north) / (2 R cloud_scale)

Adding offset D to uv1 in the vertex shader shifts uv2 by D and uv3/uv4 by
16 D. World-fixed: D accumulates the camera's motion,
D += (d east, -d north) / (2 R s).

Wrap: the cloudsF lookups use uv1/8, (uv3+uv1)/16 = 17 uv1/16,
(uv1+uv3)/4, (uv4+uv2)/8, uv1, uv2, uv3, uv4: all periodic when D grows by
16. D is kept in F64 and wrapped mod 16 on the CPU: invisible, small floats
in the shader.

Incremental, not global position / (2 R s): a global position is millions
of metres, so any cloud_scale change during a day-cycle blend would make
the clouds race. Incremental D only moves with the camera. Teleport (camera
jump > 5 km): no accumulation, clouds simply continue (no meaning in
matching the old place).

## New module `asworldclouds.cpp/.h` (AS-owned)

`ASWorldClouds` (statics):
- `update()`: once per frame (keyed on `LLFrameTimer::getFrameCount()`).
  - active = `ASVolumetricLighting::isEnabled() && isExactMode()` and the
    sky has clouds (same tests as `renderCloudShadow()`: sky, no HDRI,
    RENDER_TYPE_CLOUDS, cloud noise texture, cloud_scale >= 0.001; move
    these into a shared helper `ASWorldClouds::skyHasClouds(pipeline)` and
    call it from `renderCloudShadow()` too).
  - inactive: D = 0, last camera invalid.
  - active: camera global (`gAgent.getPosGlobalFromAgent(camera origin)`,
    x = east, y = north), delta from last frame unless first frame or
    > 5 km; D += (dx, -dy) / (2 R s); wrap mod 16 (F64).
- `layerHeight()`: `ASCloudLayerHeight` clamped 250..4000 m.
- `radius(sky)`: H / (1 - clamp(dome_offset, 0, 0.999)); used by the bake
  (replaces the `CLOUD_CEILING_HEIGHT` maths in `renderCloudShadow()`).
- `offsetAt(const LLVector3d& global)`: D + (global - camera) mapped the
  same way, for the bake anchor.
- `applyUniforms(LLGLSLShader&)`: calls update(), sets
  `as_cloud_world_offset` (vec2, (F32)D; 0 when inactive).

## Edits

1. `app_settings/shaders/class1/deferred/cloudsV.glsl` (SL, tagged): uniform
   `as_cloud_world_offset` and `vary_texcoord0.xy += as_cloud_world_offset;`
   right after the cloud_scale lines (before vary_texcoord1..3 derive).
2. `asHorizonCloudTintV.glsl` (AS): same two lines.
3. `asVolumetricCloudShadowF.glsl` (AS): same offset on uv1 in `cloudAlpha()`
   after the scale (before uv2/uv3/uv4), uniform set from `offsetAt(anchor)`.
4. `lldrawpoolwlsky.cpp` `renderSkyCloudsDeferred()` (LL, tagged): after
   `cloudshader->bind()`, `ASWorldClouds::applyUniforms(*cloudshader);`.
   Covers the vanilla and Horizon cloud shaders, reflection probe radiance
   passes and water reflection (same function).
5. `asvolumetriclighting.cpp`:
   - `CLOUD_CEILING_HEIGHT` -> `ASWorldClouds::layerHeight()`.
   - Anchor: drop the lazy slide (`CLOUD_ANCHOR_MAX_OFFSET`,
     `_SLIDE_SECONDS`, `gCloudAnchorSliding`). Anchor = camera snapped to a
     1 km global grid (F64 floor). Map content is world-fixed through
     `offsetAt(anchor)`; a re-centre only changes the sphere curvature under
     the camera (<= ~5 m at 2 km height, invisible). Keeps the map texel
     grid still between snaps (no edge shimmer while walking).
6. `settings.xml`: `ASCloudLayerHeight` (F32, 2000, persist): height of
   the cloud layer above the viewer, sets the clouds' physical size, the
   parallax and cloud-shadow speed (Exact volumetric only).
7. `CMakeLists.txt` (newview): add `asworldclouds.cpp/.h` (tagged if it is
   an LL file; follow how `asvolumetriclighting.cpp` was added).
8. Doc: new section in `doc/volumetric_lighting_exact_redesign_research.md`
   (world-fixed clouds: maths, wrap, decisions, verification). Plan copied to
   `doc/ayanestorm-world-fixed-clouds-plan.md` on approval.

Shaders validated with `.glslang` (V as .vert, F as .frag, `#version 410
core` prepended). No shader revision bump.

## Verification (user build)

- Exact on, clouds frozen (scroll 0): walk/fly east -> overhead clouds slide
  west across the sky (parallax), horizon clouds barely move. If they move
  with the camera or the wrong way, a sign is off (one-line fix).
- Cross a region border: no jump. Teleport: no glitch.
- Debug 4 on open ground: shadow patches stay put while walking 1 km, a far
  hill lights before/after the user as a cloud passes (scroll on).
- Cloud in front of the sun <-> user in shadow, also after walking 1 km.
- `ASCloudLayerHeight` 500 vs 2000: smaller clouds/faster parallax at 500,
  shadows still aligned with the drawn clouds.
- Exact off: clouds camera-attached as vanilla. Toggle Exact: one jump, fine.
- Water reflection and reflection probes: clouds consistent with the sky.
- Pending: debug 7 bands, debug 0 rays with Strength 10..50, softer cloud
  shadow edges (mip fix).
- Snapshot: clouds and shadows match the live view.
