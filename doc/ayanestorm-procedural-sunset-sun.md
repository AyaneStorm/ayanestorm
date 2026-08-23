# AyaneStorm procedural sunset sun

## Purpose

Some EEP skies provide solar illumination and haze but use an absent or
transparent sun texture. The optional viewer-local procedural sun fills those
transparent pixels near sunrise and sunset without changing the EEP asset.

## Rendering design

The existing sun billboard remains the geometry and render-order authority. A
soft analytic circle is composited underneath its EEP texture, so opaque
authored suns remain unchanged. The pass stays after sky haze and before clouds,
and retains the existing far depth and scale-aware partial-horizon cutoff.

When the feature is enabled, the billboard also stops applying the legacy
SL-10303 50-meter downward displacement from `sunDiscV.glsl`. That workaround
visibly separates the disc from the true EEP sun direction near the horizon.
Disabled mode retains the displacement for upstream-compatible rendering.

The disc fades in from the configured start elevation, peaks at 2 degrees, and
dims to zero when the billboard's upper edge passes below the horizon. Its
starting chroma is the normalized live EEP sunlight color. The core remains
mostly white-hot at low elevations, while the configured final color moves into
the outer limb and feather toward the lower cutoff. Broad orange-red emission
continues to come from the EEP atmosphere rather than tinting the entire disc.
Brightness is viewer-local and does not affect scene lighting, shadows, god
rays, or water reflections.

## Settings and validation

`ASProceduralSunEnabled`, `ASProceduralSunStartAngle`,
`ASProceduralSunFinalColor`, `ASProceduralSunBrightness`,
`ASProceduralSunFeather`, `ASProceduralSunShimmer`,
`ASProceduralSunHaloStrength`, and `ASProceduralSunHaloRadius` are exposed in the Sun
Settings floater and apply live. Feathering widens the analytic limb transition.
Shimmer applies two low-amplitude animated refraction waves as the scale-aware
lower limb enters the five-degree horizon band and does not alter the actual
light direction. The feather transition and shimmer margin remain entirely
inside the finite sun billboard so the quad cannot clip them into a hard edge.
While the procedural
feature is active, both controls shape the final analytic silhouette, including
opaque EEP texture pixels; authored texture color and interior detail remain.
The halo is a separate enlarged, additive, depth-tested billboard drawn behind
the disc. It fades in from two degrees to the horizon, uses the configured warm
limb color, does not write depth, and remains occluded by subsequently rendered
clouds.
Validate absent, transparent, partially
transparent, and opaque EEP sun textures; multiple sun scales; HDR and non-HDR;
cloud occlusion; haze blending; foreground depth; and elevations around the
start, 2 degrees, zero, and the derived lower-edge cutoff.
