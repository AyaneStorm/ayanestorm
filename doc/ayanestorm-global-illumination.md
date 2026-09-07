# AyaneStorm Global Illumination

Author: chanayane@firestorm

## Implemented Probe-Based Indirect Lighting

Source review confirms approximate global illumination through reflection probes. `indra/newview/llreflectionmapmanager.cpp`, `LLReflectionMapManager::updateProbeFace()`, documents six direct-light capture passes producing irradiance, then six passes including irradiance producing radiance. The code describes this as simulated single-bounce lighting.

`indra/newview/app_settings/shaders/class1/deferred/deferredUtil.glsl`, `pbrIbl()`, uses probe irradiance for diffuse indirect lighting and radiance for specular lighting. `class3/deferred/reflectionProbeF.glsl` samples the irradiance cube-map array.

## Scope and Limitations

This is spatially approximated probe lighting, not a per-surface multi-bounce path-tracing solution. No SSGI, path-tracing, or voxel-cone GI implementation was found by a source search. Probe update scheduling can delay lighting changes. Actual contribution depends on environment and probe configuration; this review does not establish the running viewer settings.

## Existing UI Terminology

`skins/default/xui/en/floater_advanced_lighting.xml` calls the ambient probe contribution global illumination and warns that ambient changes can take seconds to recompute. Related tooltips occur in `floater_advanced_phototools.xml`.

No source changes, build, or runtime testing were performed for this review.
