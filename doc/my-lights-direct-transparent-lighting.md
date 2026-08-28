# My Lights direct-backend transparent lighting

## Symptom

The direct shader backend initially produced darker alpha hair and other
forward-rendered transparent objects than the `LLVOVolume` backend. Opaque skin
and scene surfaces were substantially equivalent.

## Cause

Direct lights were submitted only to the deferred point-light pass.
`LLVOVolume` lights also enter `LLPipeline::mNearbyLights`, from which
`LLPipeline::setupHWLights()` fills local hardware-light slots 2 through 7.
Forward transparency shaders consume those uniforms, so they could not see the
viewer-object-free lights.

## Resolution

`ASLightRigRenderer::appendForwardLights()` now configures the same hardware
light state for direct lights, matching the stock point-light calculations for
linear color and intensity, position, adjusted radius, falloff, linear and
quadratic attenuation, size, and omnidirectional flags. Direct My Lights reserve
the available local slots before ordinary nearby lights so alpha content cannot
lose them merely because no drawable participates in nearby-light sorting.

## Runtime verification

Compare both backends without moving the camera or changing the rig. Pay
particular attention to alpha hair strands, lashes, sheer clothing, jewelry,
and opaque skin adjacent to them. Their local-light response should now closely
match. Ordinary nearby lights can differ when more than six local lights compete
for forward slots because direct My Lights are deliberately reserved first.
