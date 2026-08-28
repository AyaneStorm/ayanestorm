# My Lights isolate background newcomer avatars

## Symptom

Avatars arriving after a solid My Lights background is enabled can remain
visible. Toggling My Lights or switching through Normal Scene does not reliably
remove them.

## Cause

The live isolate filter correctly classified the avatar and set
`LLDrawable::FORCE_INVISIBLE`. That flag is sufficient for volume geometry only
after its spatial-group batch is rebuilt. Avatar drawables are non-volume:
`LLPipeline::stateSort(LLDrawable*, LLCamera&)` skipped their `setVisible()` call
when the flag was present, but then continued and enqueued their faces later in
the same function. A newly arrived avatar could therefore remain renderable
despite having the correct hidden flag.

## Resolution

`ASBackgroundIsolate::updateDrawableHiddenState()` now returns the current hide
decision. The existing tagged pipeline call-out returns immediately when that
decision is true. Volume drawables still receive the same flag and group rebuild;
non-volume avatar faces can no longer proceed to face enqueueing. Shadow passes
continue to receive `false` because the isolate module deliberately restores
hidden geometry while rendering shadows.

## Runtime verification

Enable a black, white, or custom background in a populated region and wait for
previously unseen avatars to arrive. Confirm that neither their bodies nor their
attachments appear. Repeat after switching through Normal Scene, and confirm
that all avatars return in Normal Scene while newcomers remain hidden after
isolate mode is re-enabled.
