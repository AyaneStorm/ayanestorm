# Local Light Priority (RenderLocalLightCount, ASRenderLocalLightSubjectSort)

## Upstream behavior

- Container: `LLPipeline::mNearbyLights`, `light_set_t = std::set<Light, Light::compare>` (`pipeline.h`, protected).
- `Light::compare`: `dist` ascending, tie by `drawable` address. `fade` is not part of the key.
- `calc_light_dist()` (`pipeline.cpp`): intensity < 0.001 -> `max_dist`; selected -> 0; else `max(|renderPos - cameraOrigin| - radius, 0)`; moving (ACTIVE) -> minus `0.25 * radius`.
  The distance is from the camera to the edge of the light's sphere of influence. Every light containing the camera scores 0, so their order is effectively arbitrary.
- `calcNearbyLights()` rebuilds the set each frame from `mLights`. In deferred mode `max_dist = RenderFarClip` and there is no cap. Lights out of range fade out (`fade < 0`).
- `renderDeferredLighting()` walks the set and stops after `RenderLocalLightCount`. The counter increments BEFORE the attachment, zero-radius, dark-color and frustum rejects, so rejected lights still use up slots.
- Point and spot lights share one ordering, and the cone is ignored.
- Worn (attached) lights:
  - They are excluded entirely when `RenderAttachedLights` is false, both in `calcNearbyLights()` (existing and new lights) and again in the cap loop. HUD lights are always excluded.
  - Lights on avatars that are too complex, muted or too slow (jellydolled) are dropped from `mNearbyLights` in `calcNearbyLights()`.
  - Otherwise they are ordinary entries. Attachments are ACTIVE drawables, so they always get the moving-light bonus (`- 0.25 * radius`). Position is `getRenderPosition()` of the light prim, so it follows the attachment point and animation.
- My Lights (`ASLightRig`). Which path they take depends on `ASLightRigRenderBackend`:
  - `LLVOVolume` (the default): each light is a viewer-local `LLVOVolume` created with `createObjectViewer` and positioned with `setPositionAgent`. It is not an attachment, so `RenderAttachedLights` does not apply. It enters `mLights` and `mNearbyLights` like any light, shares the `RenderLocalLightCount` budget, and is ordered by the subject sort when that is on.
  - `Shader`: `ASLightRigRenderer::render()` runs after the stock lights with its own list and a separate cap of `min(size, RenderLocalLightCount)`. It does not share the budget, is not affected by the subject sort, and its index counts off-screen lights too. `RenderLocalLightCount <= 0` disables it. The forward path uses `appendForwardLights()`.
- The forward path (`setupHWLights`, slots 2-7) and `asvolumetriclighting.cpp` also walk `mNearbyLights`. Neither is affected by this change.

## Camera focus / subject point

- `LLViewerCamera::getPointOfInterest()` returns `mLastPointOfInterest`, in agent coordinates. It is set by `updateCameraLocation()` from `gAgentCamera.getFocusGlobal()`.
- DoF builds its own focus point (`sLastFocusPoint` in `renderDoF`). It matches the camera focus only in the default case; lock, follows-pointer, mouselook and media focus differ. It also runs after `calcNearbyLights`, so it lags one frame.
- The camera focus is NOT the Alt-click hit:
  - `LLToolCamera::pickCallback()` (`lltoolfocus.cpp`) runs on Alt-click or with the Camera tool. It calls `pickAsync(pick_transparent=false, pick_rigged=false)`, then `setFocusOnAvatar(false)` and `setFocusGlobal(pick_info)`.
  - `LLPickInfo::mPosGlobal` is the ray hit under the pointer (`llviewerwindow.cpp`). Land hits are lifted 0.1 m.
  - `LLAgentCamera::setFocusGlobal(const LLPickInfo&)` uses the hit only for land and GLTF nodes. On objects it uses `object position + mObjectOffset`, where `calcFocusOffset()` projects the pointer onto a plane through the object's center (meant for tumbling). On avatars it uses the hit relative to `getCharacterPosition()`.
  - `mFocusGlobal` animates toward `mFocusTargetGlobal`. When focus returns to the avatar (`mFocusOnAvatar`), it becomes avatar-relative (`calcFocusPositionTargetGlobal()`).
  - Rigged mesh is not picked (`pick_rigged=false`), so on mesh bodies the hit lands on the avatar's collision volumes rather than the visible skin.

## ASRenderLocalLightSubjectSort (default false)

- Module: `aslocallightsort.h/.cpp`, class `ASLocalLightSort` (friend of `LLPipeline`).
- Subject: `ASLocalLightSort::setSubject(pick_info)` is called from tagged lines in `LLToolCamera::pickCallback()`, next to both `setFocusGlobal(pick_info)` calls (object and land). It stores `mPosGlobal`, plus the object ID and the hit in the object's render frame, so the subject follows a moving object or avatar. If the object is gone, the last known global position is used.
- Lifetime: the subject lasts while `gAgentCamera.getFocusOnAvatar()` is false. It is dropped when focus returns to the avatar (walk, Esc, camera reset), and replaced by the next Alt-click. A focus change that is not an Alt-click or Camera tool pick (for example menu zoom-to-object or sit camera) does not update it.
- No subject: the camera origin is used, which gives upstream order (sorting still applies, but scores match `calc_light_dist()`).
- When on, the cap loop iterates `mNearbyLights` in order of `max(|renderPos - subject| - radius, 0)`. Selected lights score 0, moving lights get the 0.25 * radius bonus, dim lights sort last, and the tie-break is the drawable address. Lights fading out of camera range sort after in-range lights.
- When on, `count` increments only after the off-screen and dark checks, so only drawn lights use up the cap.
- Worn lights get no special case. They are scored from the subject like any other light, keeping the moving-light bonus. If the subject is on an avatar, that avatar's own worn lights score about 0 and are kept first. Other avatars' face lights, including your own when the subject is someone else, rank by their distance from the subject.
- When off, the set order and counting are the same as upstream.
- Candidate selection, range test and fading are unchanged and remain camera-based.
