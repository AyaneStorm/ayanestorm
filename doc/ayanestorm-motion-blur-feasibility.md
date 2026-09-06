# Optional motion blur feasibility

Author: chanayane@firestorm
Date: 2026-09-06

## Current implementation and SMAA

Searches found motion blur references in SMAA integration documentation, not an implemented viewer blur pass. `indra/newview/app_settings/shaders/class1/deferred/SMAA.glsl:324` describes integrating an externally supplied motion blur pass. `indra/newview/llviewershadermgr.cpp:2759` explicitly defines SMAA_REPROJECTION as 0. Conditional velocityTex declarations in SMAANeighborhoodBlendF.glsl therefore do not establish active velocity support.

## Feasible first scope: camera motion blur

Use scene depth, inverse current view-projection, and previous main-view view-projection to reconstruct screen displacement caused by camera movement. Sample scene color along that displacement with depth rejection and a bounded radius. This does not recover independent object movement or avatar animation.

`LLPipeline::renderFinalize()` in `indra/newview/pipeline.cpp:9090` already provides ping-pong postprocessing targets, DoF, FXAA/SMAA and later overlays. A candidate prototype hook is before FXAA/SMAA, with final ordering against DoF, glow and tone mapping chosen after visual testing. Keep substantial implementation in a new AyaneStorm module and shader, with minimal tagged integration edits.

Default off; expose strength and quality. Skip the pass and additional resources when disabled. Track main-view history only; invalidate on enable, teleport, camera cuts, origin shifts, projection/viewport changes and rendering context recreation. Handle long frame gaps, snapshots, reflections, sky and HUD attachments explicitly. Control exposure duration relative to frame time and cap blur distance.

## Full object motion blur and transparency

Full motion blur additionally needs previous object transforms and previous deformation/skinning state, plus motion-vector generation. That is substantially more invasive across render paths. Transparent hair, glass, particles and OIT can contain several depths and motions per pixel; one resolved color and depth cannot represent them faithfully. Existing OIT DoF work documents analogous depth-layer limitations in `doc/ayanestorm-oit-depth-of-field-transparent-depth.md`.

## Cost and validation

Camera blur adds a fullscreen sampling pass; cost scales with resolution and sample count. No measured GPU cost is available. Full object blur adds geometry/state/buffer costs. Validate camera rotation/translation, moving avatars with a stationary camera, transparent layers, DoF, AA, HDR modes, sky, HUD attachments, snapshots and camera discontinuities. No build or runtime test performed for this assessment.

## Reference

NVIDIA GPU Gems 3, Chapter 27, Motion Blur as a Post-Processing Effect: https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-27-motion-blur-post-processing-effect

Describes depth reconstruction and previous-camera reprojection, and distinguishes camera motion from independently moving objects. The integration proposal above is an inference from this technique and the local rendering code.
