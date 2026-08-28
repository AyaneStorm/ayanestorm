# My Lights login restoration

## Symptom

Enabled autosaved lights do not appear after login until the user opens the
My Lights floater.

## Cause

`LLFloaterReg` constructs registered floaters lazily. Autosave loading and the
My Lights idle callback are initialized by `ASFloaterMyLights::postBuild()`, so
neither runs before the floater's first construction.

## Resolution

The completed-login cleanup state now calls
`LLFloaterReg::getInstance("as_my_lights")`. This constructs the instance without
showing it, loads the per-account autosave, and registers the idle callback after
the account path and self-avatar are available. If avatar creation is unusually
late, the idle callback creates enabled lights once the avatar becomes valid.

## Runtime verification

Enable at least one light, quit normally so the rig autosaves, then log in
without opening My Lights. Verify that the light affects the scene after the
avatar appears. Open the floater afterward and verify that its list and controls
match the restored lights.
