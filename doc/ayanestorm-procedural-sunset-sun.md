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
holds most of that brightness through the horizon crossing before a short,
smooth fade to zero when the billboard's upper edge passes below the horizon. Its
starting chroma is the normalized live EEP sunlight color. The complete disc
remains uniformly bright and mildly warm at low elevations; feathering changes
only its optical coverage and does not create an interior red ring. The
configured final color drives the separate atmospheric halo and the disc's
restrained elevation-dependent warming. Broad orange-red emission continues to
come primarily from the EEP atmosphere rather than a radial disc gradient.
Brightness values from zero to one transition the disc from the configured
final color to its white-hot live EEP color instead of multiplying it toward
black. This transition uses a sunset-specific chroma curve that attenuates blue
before green, providing a broad yellow/orange range instead of a white-pink-red
RGB interpolation. Values above one boost emitted intensity. The enable
checkbox, rather than zero brightness, hides the procedural disc. The setting remains
viewer-local and does not affect scene lighting, shadows, or god rays. Water
continues using the EEP sun direction and sunlight color, but retains that sun
as its specular source until the procedural disc reaches its scale-aware cutoff
instead of switching to moonlight when the sun center crosses zero elevation.

## Settings and validation

`ASProceduralSunEnabled`, `ASProceduralSunStartAngle`,
`ASProceduralSunFinalColor`, `ASProceduralSunBrightness`,
`ASProceduralSunFeather`, `ASProceduralSunShimmer`,
`ASProceduralSunHaloStrength`, `ASProceduralSunHaloRadius`, and
`ASProceduralSunHaloSoftness` are exposed in the Sun
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
limb color and a broad radial profile, does not write depth, and remains
occluded by subsequently rendered clouds. Its radius may extend to eight disc
radii. Halo softness independently controls how much of that radius is used for
the progressive fade to true zero.
The halo billboard is enlarged strictly around the arithmetic center of the
four sun-billboard corners, keeping the glow concentric with the rendered disc.
Its RGB is premultiplied and additively composited with zero glow-mask alpha.
This keeps the analytic halo visible and live-configurable without generating a
second blurred post-process halo.
While the procedural disc is active, its visible RGB uses normal alpha blending
but destination alpha is preserved. Because scene alpha is the post-process
glow mask, this prevents the bright fallback disc from generating another
automatic halo beside the one controlled by the Sun Settings floater.
Fully feathered disc fragments are discarded before MRT output so the
categorical skip-atmosphere metadata cannot reveal the otherwise transparent
billboard boundary.
Validate absent, transparent, partially
transparent, and opaque EEP sun textures; multiple sun scales; HDR and non-HDR;
cloud occlusion; haze blending; foreground depth; and elevations around the
start, 2 degrees, zero, and the derived lower-edge cutoff.
