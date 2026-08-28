# My Lights isolate-background FPS cost

## Symptom

FPS can be lower when **My Lights** uses a solid background instead of
**Normal scene**, even though most scene geometry is hidden.

## Primary cause

Isolate mode adds expensive CPU and geometry-management work in
`LLPipeline::stateSort()`:

- Occluded spatial groups are treated as visible so their drawable hidden
  states cannot become stale.
- Every drawable in each visible spatial group is state-sorted every frame,
  bypassing the normal `changeLOD()` gate.
- `ASBackgroundIsolate::updateDrawableHiddenState()` changes
  `LLDrawable::FORCE_INVISIBLE` and dirties/requeues the owning spatial group
  whenever the desired hidden state changes.
- Shadow passes deliberately reveal scene geometry so it can still cast
  shadows onto the avatar. The main-camera pass hides it again. Consequently,
  drawable flags can toggle and spatial groups can be rebuilt repeatedly
  between shadow and main-camera passes.

This work scales with the surrounding scene's drawable and spatial-group
count. Hiding the final pixels therefore does not necessarily make the frame
cheaper.

## Secondary GPU costs

Active isolate mode also adds two full-screen background shader passes: an HDR
base layer before transparency and a final exact-color pass after post
processing. ExactOIT or AVBOIT additionally performs an isolate-only depth pass
for transparent-pixel coverage. These costs scale mainly with display
resolution and are likely secondary to repeated scene traversal and geometry
rebuilds in content-heavy regions.

## Profiling direction

Compare CPU time in state sorting and spatial-group geometry rebuilding between
Normal scene and isolate mode. Also compare with shadows disabled and with a
low-content region. A disproportionately large improvement from either test
would confirm the traversal/rebuild path as the main bottleneck.
