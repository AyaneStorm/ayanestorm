# AyaneStorm Vignette Implementation

## Overview

The viewer-local vignette is an independent multiplicative fullscreen pass in
`ASVignette`. It runs after the additive lens-flare pass and before snapshot
guides and UI overlays. Ordinary snapshots include it; cube snapshots do not.

## Controls and Semantics

- `ASVignetteEnabled` defaults off.
- `ASVignetteStrength` ranges from 0 to 1 and defaults to 0.50. Zero is a
  rendering no-op; one permits the outer region to become fully black.
- `ASVignetteRadius` ranges from 0 to 10 and defaults to 1. At one, the circle's
  diameter equals the shorter viewport dimension. Zero darkens the whole image.
- `ASVignetteSmoothness` ranges from 0 to 1 and defaults to 0.50. Zero produces
  a hard boundary; one fades from the center to the radius.
- `ASVignetteShape` ranges from -1 to 1 and defaults to 0. Zero is circular,
  -1 stretches the major axis horizontally to twice its circular size, and 1
  stretches it vertically to twice its circular size.

## Rendering

The fragment shader outputs a grayscale multiplier and uses multiplicative
blending against the completed 3D framebuffer. Coordinates are normalized by
the shorter viewport dimension so circular shapes remain circular at every
aspect ratio. This avoids another framebuffer copy or texture sample.
