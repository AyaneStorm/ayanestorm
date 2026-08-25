# Optional Aurora / Northern Lights Feasibility

## Conclusion

`auroras.shader` is enough as visual and algorithmic inspiration, but it is not
enough as an implementation for AyaneStorm. It cannot be installed as-is and
should not be copied into the viewer without separate permission from its
author.

## What the Reference Provides

The useful core is the procedural curtain model:

- five-octave triangular noise (`triNoise2d`);
- animated distortion;
- a sparse volumetric integration with increasing sample distance;
- sample jitter to reduce banding;
- green/blue/red emission variation along the curtain.

The background, stars, water-like reflection, mouse camera, and Shadertoy
camera setup are demo scaffolding and are not appropriate for viewer
integration. AyaneStorm already renders its atmosphere, stars, camera, and
water.

## Why It Is Not Drop-In Source

- It uses Shadertoy uniforms (`iResolution`, `iTime`, `iMouse`) and a
  `mainImage()` entry point rather than the viewer shader interface.
- The copied file contains explanatory text, the invalid placeholder
  `samplerXX iChannel0..3`, and trailing numbered duplicate lines, so it is not
  valid GLSL as stored.
- Its ray origin and direction are constructed from a demo camera. The viewer
  must derive a stable world/view ray from the sky dome and camera.
- The demo replaces the background and implements a fake lower-half
  reflection. A viewer aurora should composite over the existing sky and let
  the existing water/reflection systems handle reflections.
- The fixed 50-step loop, with five noise octaves per step, is expensive at
  sky-screen resolution. It needs profiling and quality tiers.
- It has no controls, shader registration, C++ render pass, deferred-buffer
  output policy, cube/reflection-probe policy, or UI.

## Licensing Blocker

The reference declares **Creative Commons Attribution-NonCommercial-ShareAlike
3.0 Unported** and says to contact the author for other licensing options.
AyaneStorm's viewer shaders are distributed under the viewer LGPL framework.
The non-commercial and share-alike restrictions should be treated as
incompatible with directly incorporating or adapting this code unless the
author grants suitable written permission.

Use the visual concept only and write an independently implemented algorithm,
or obtain an LGPL-compatible grant before porting any code. Preserve evidence
of any grant in the project documentation.

## Recommended Architecture

Implement the effect as an AyaneStorm-owned module and dedicated shader, not by
growing the upstream sky fragment shader:

- `asaurora.h/.cpp`: settings, night visibility, render parameters, and the
  isolated render call;
- dedicated `asauroraV.glsl` and `asauroraF.glsl` shader files;
- one narrowly tagged call from `LLDrawPoolWLSky::renderDeferred()`, after sky
  haze and heavenly bodies/stars but before clouds;
- narrowly tagged shader registration declarations/initialization in the
  upstream shader-manager files;
- viewer-local settings, defaulting disabled;
- an AyaneStorm-owned settings panel or controls.

A distinct shader program is preferable to a runtime branch inside
`skyF.glsl`: when disabled, it incurs no full-screen aurora fragment cost and
does not complicate the heavily merged upstream atmospheric shader.

## Minimum Viable Controls

- Enable aurora (default `false`)
- Intensity
- Animation speed
- Curtain scale / density
- Height and horizon fade
- Color tint or a small color preset
- Quality: low / medium / high, controlling integration steps and noise octaves

The first version should be viewer-local. Adding aurora fields to EEP assets
would require a nonstandard serialization format and interoperability policy,
so it is a separate feature.

## Rendering Requirements

- Fade by sun elevation so auroras appear at night and transition smoothly.
- Use a world-anchored coordinate system to prevent the pattern following or
  swimming with the camera.
- Restrict the effect to the upper hemisphere and fade it at the horizon.
- Output additive/emissive color while respecting the viewer's deferred-buffer
  layout and glow-mask semantics.
- Render before clouds so clouds can occlude or attenuate the aurora.
- Decide explicitly whether it appears in cube snapshots, reflection probes,
  water reflections, and photography snapshots. Start by omitting expensive
  irradiance/probe passes, matching the repository's existing care around sky
  effects.
- Avoid the reference shader's synthetic stars, background, and reflection.

## Performance Starting Point

Start below the reference's 50 steps. Suggested experimental tiers are 12, 20,
and 32 integration samples, with 3, 4, and 5 noise octaves respectively. Add
stable per-pixel or blue-noise jitter and temporal animation. These values are
starting hypotheses and must be runtime-tested on representative GPUs.

Potential later optimizations include rendering to a reduced-resolution target
and compositing, temporal reprojection, or replacing some procedural octaves
with a small repeatable noise texture.

## Implementation Readiness

Before implementation, settle two product choices:

1. viewer-local global preference versus per-sky preset behavior;
2. acceptable license path: independent clean implementation versus written
   permission to adapt the reference.

With viewer-local settings and an independent implementation, the repository
already has suitable sky-dome and render-pool hooks. No new asset or simulator
support is required for a first version.

## Implemented Baseline (2026-08-25)

The first clean-room AyaneStorm implementation now consists of:

- `asaurora.h/.cpp`, owning its shader program and live setting transfer;
- original value-noise curtain shaders, with no source taken from the visual
  references;
- a single sky-pool draw hook between stars and clouds;
- an optional shader-manager lifecycle hook which cannot fail the core shader
  load if a driver rejects the aurora program;
- a dedicated Aurora Settings floater opened from AyaneStorm Rendering
  preferences;
- persistent controls for enable, quality, intensity, speed, curtain scale,
  height, thickness, daylight fade, and independently configurable lower and
  upper colors.

The effect defaults off, is omitted from cube snapshots, uses stable global
camera coordinates for world anchoring, and performs no rendering work while
disabled or faded out by daylight. Runtime build and visual tuning remain for
the user according to repository policy.

## Complete Visual Reference Set

The four supplied files were reread in their complete form on 2026-08-25.
Together they demonstrate a multi-buffer presentation rather than four
independent aurora implementations:

- `auroras.shader` generates volumetric-looking animated curtains along with
  its own demo sky, stars, and synthetic reflection.
- `auroras2.shader` consumes an input texture, creates three radial/zoom-blur
  layers, colors them differently, and adds a procedural cell background. Its
  blur layers total roughly 80 texture reads per pixel before other work.
- `auroras3.shader` is a variant of the first reference with altitude-driven
  red, green, and blue emission profiles.
- `auroras4.shader` consumes an input texture and applies a 4-by-4 spatial blur,
  gamma-like brightening, and a vignette.

The corrected high-level visual targets are therefore layered spectral color,
elongated luminous rays, and a soft glow around a sharper curtain core. The
AyaneStorm baseline already supports altitude-separated user colors and a
smooth emissive core. Radial stretching and softness could be pursued later
with original reduced-resolution post-processing, but copying the reference
multi-pass design would add excessive sampling cost and is unnecessary for the
first runtime evaluation.

## First Runtime Visibility Diagnosis

The 2026-08-25 runtime log confirmed `ASAuroraEnabled=true`, successful general
shader loading, and no aurora compile/link error. It did show an XUI parse
warning for the quality combo label, which was corrected.

The actual visibility fault was depth state: the aurora initially depth-tested
its dome after the atmosphere and celestial passes had written sky depth. It
now retains the physically meaningful order—sky haze, celestial bodies, stars,
aurora, then clouds—but renders its additive dome with depth testing and writing
disabled. Thus distant celestial objects remain behind the upper-atmosphere
aurora while nearby clouds remain in front.

The aurora fragment output was hardened to follow the viewer's
`HAS_EMISSIVE` deferred permutation, matching the established sky shader output
policy. During active development, shader-cache invalidation remains manual,
following the repository's established policy. Runtime retesting should use a
night EEP; by design, the configured sun fade suppresses the effect in daylight.

The subsequent root-cause trace found that merely supporting the fragment
permutation was insufficient: unlike the upstream sky programs, the independent
aurora program never enabled `HAS_EMISSIVE` when
`RenderEnableEmissiveBuffer` was active. It consequently wrote to attachment 0
while the active sky pipeline consumed attachment 3. Shader creation now clears
old permutations and explicitly mirrors the saved emissive-buffer selection.

## `sunset3.shader` Integration Reference

The integrated sunset reference was reviewed on 2026-08-25. Its aurora
generator remains closely related to the earlier visual references, but its
final composition gates the effect by solar position and modulates it with
atmospheric/cloud scattering and reflection attenuation. AyaneStorm already
achieves the appropriate viewer-native equivalents through sun-elevation fade,
placement between celestial bodies and clouds, and existing water rendering.

The first screenshot exposed a separate AyaneStorm baseline problem: every sky
direction sampled the fine curtain field, producing nearly continuous auroras.
An original broad occupancy envelope now groups fine curtains into localized
displays. The new live `ASAuroraCoverage` control ranges from no occupied sky at
zero to broad coverage at one and defaults to a sparse `0.35`.

The Ultra quality tier extends integration to 64 samples. Low, Medium, and High
remain 12, 20, and 32 samples. Following runtime tuning, Ultra is the default;
it can approximately double aurora fragment sampling cost relative to High.

Post-Ultra attempts to evaluate coverage once and reject integration early were
reverted after runtime regressions: the shell-center version formed detached
horizontal ellipses, and the azimuth version could suppress the display. The
working per-sample coverage evaluation is restored for all quality tiers.

A persistent `ASAuroraSeed` selects stable coverage and fine-curtain noise
domains. The Aurora Settings floater exposes both the numeric seed and a
`Randomize seed` action, allowing instant arrangement changes without affecting
quality or meaningfully changing rendering cost.
