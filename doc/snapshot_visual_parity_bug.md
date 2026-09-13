# Snapshot Visual Parity

## Symptoms

A high-resolution snapshot rendered brighter than the live view and its color-grading grain nearly disappeared after the snapshot was reduced to the live-view display size.

## Causes

- `LLPipeline::renderFinalize()` regenerated dynamic HDR exposure from the snapshot render. A different snapshot aspect ratio or render target could therefore change tone mapping despite unchanged camera and graphics settings. Tiled captures could also adapt between tiles.
- Color-grading grain size was measured in render-target pixels. The same 1--8 pixel cells were used for both the live target and a much larger snapshot, so reduction made snapshot grain proportionally smaller.

## Resolution

- Snapshot rendering reuses the live view's existing exposure map. Normal frames continue to update dynamic exposure.
- The color-grading presentation pass remembers the last live target dimensions and supplies a snapshot resolution scale. Grain cell size is multiplied by that scale. Tiled captures use their camera zoom when it is the larger scale.

This preserves live-view tone mapping and apparent grain size while leaving final presentation dithering at native output-pixel resolution.

## Verification

Build and runtime testing are intentionally left to the user per repository policy. Compare a live screenshot with same-camera snapshots at viewport resolution and 6000x6000, reducing the latter to the live display size. Check brightness, grain size, and tile seams.
