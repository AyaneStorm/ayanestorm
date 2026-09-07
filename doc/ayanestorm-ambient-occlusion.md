# Ambient Occlusion (SSAO)

Author: chanayane@firestorm

## Implementation and settings

The viewer implements screen-space ambient occlusion (SSAO).
`RenderDeferredSSAO` defaults to true in `indra/newview/app_settings/settings.xml`.
`indra/newview/llviewershadermgr.cpp` selects `deferred/sunLightSSAOF.glsl` when enabled.

## User controls

The Graphics preferences panel exposes Ambient Occlusion (`UseSSAO`).
Phototools exposes Enable Ambient Occlusion (Depth Perception) and adjustment controls.
Sources: `indra/newview/skins/default/xui/en/panel_preferences_graphics1.xml` and `floater_phototools.xml`.

Repository inspection only; the current runtime setting was not checked.
