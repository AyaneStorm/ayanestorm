# AyaneStorm New Renderer: Requirements

This document states what the new renderer must do. It does not prescribe how. Architecture, data structures, threading, pass order, shader design and file layout are the developer's decisions.

## Goal

An optional modern renderer for AyaneStorm that is much faster than the OpenGL renderer and keeps AyaneStorm's image quality.

## Project constraints

- **Optional:** the OpenGL renderer stays complete, stays the default until the user decides otherwise, and remains the fallback.
- **Selection:** a setting chooses the renderer. A restart to switch is acceptable. New setting keys start with `AS`.
- **Firestorm merges:** upstream merges must stay practical. Put new work in new AyaneStorm modules and keep edits to `ll*` and `fs*` files minimal and tagged, as `AGENTS.md` requires.
- **Language and licence:** C++, compatible with LGPL 2.1, with FMOD and Kakadu builds still possible.
- **Platforms:** Windows and Linux on Vulkan, macOS on Apple Silicon. macOS must have a native path, not a best-effort one.
- **Main loop:** no block above about 30 seconds inside `display()`, or the viewer logs out.
- **Building:** the user runs all builds. Each rebuild costs about 10 minutes, so verify before asking for one.

## Performance targets

| Measure | Target |
|---|---|
| Frame rate | As high as possible; speed is the first design priority, with no fixed ceiling |
| Busy indoor scene, about 16,000 objects, RTX 4090, frame cap off | Minimum 5x the OpenGL frame rate at the same spot and camera |
| Same scene, reference value | OpenGL 68 fps, new renderer 340 fps as a floor, not a goal |
| Arriving in a busy region | The view stays responsive while content loads |
| Frame pacing | No regular stutter from asset streaming |
| Memory | Stays within a budget derived from detected video memory |

The reference is one informal measurement. Repeat the OpenGL baseline on the test machine before using it.

## Inputs

The renderer must accept what the viewer already holds:

- Prims, sculpts and mesh with level of detail, including linksets and animesh.
- Avatars: skeletons, Bento, rigged mesh, server bakes and attachments.
- Terrain, classic and PBR.
- Water, sky, clouds, sun, moon and stars from EEP environments.
- Legacy and PBR materials, alpha modes, animated textures, glow and fullbright.
- Local lights, projectors, particles, HUD attachments and media textures.
- Camera, viewport, window size and all relevant graphics settings.

Objects may appear, move, change or disappear on any frame. Textures and meshes arrive in stages and at increasing detail.

## Outputs

- The presented frame, including the full viewer UI.
- World and face picking, and depth under a screen point.
- Snapshot capture at any supported size, without visible interruption.
- Preview images for existing floaters, such as thumbnails and dynamic textures.
- Frame statistics and GPU timings for diagnostics.

## Required behaviour

- **Visual parity:** lighting, materials, shadows, reflections, water, sky and alpha ordering match the OpenGL renderer closely, using the same settings.
- **UI:** all existing floaters and the login screen render correctly, with no rewrite of LLUI.
- **Selection and editing:** highlights, beacons, build tools and debug displays work.
- **Snapshots:** high-resolution snapshots work at up to 11500x11500. Depth of field must work in snapshots first; snapshot support is never deferred.
- **Stereo:** not required.
- **Device loss:** the viewer recovers or exits cleanly, with a useful log message.
- **Unsupported hardware:** the viewer falls back to OpenGL with a clear message.

## AyaneStorm features to carry over

All of these must work. Order is the developer's choice, and each must meet its existing quality and snapshot behaviour. A feature need not be ported as it is: where a built-in Vulkan or platform capability gives an equal or better result, use it instead.

- Depth of field: Live, aperture-sampled, autofocus, subject tracking.
- Transparency modes: Exact OIT, AVBOIT, Mac OIT and their DoF bins.
- GTAO and VBAO.
- Volumetric lighting, including Exact mode and cloud shadows.
- Motion blur, colour grading, `.cube` LUTs, lens flares, vignette, chromatic aberration and bloom.
- Night sky, sunrise and sunset, sunset sun, moon, auroras and snow.
- My Lights and light rigs.

## Acceptance

- Target scene measured against OpenGL on the same machine.
- Side-by-side captures of a fixed set of scenes covering each section above.
- Login, teleport, snapshot, alpha-heavy avatar and resize tests pass on Windows, Linux and macOS.
- Firestorm merge conflict surface measured: count of changed `ll*` and `fs*` files.
- The OpenGL renderer behaves as before when the new renderer is off.
