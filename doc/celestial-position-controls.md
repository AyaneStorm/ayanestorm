# Celestial Position Controls

## Existing implementations reviewed

Sun and moon azimuth/elevation behavior was compared in:

- `LLFloaterEnvironmentAdjust` (`llfloaterenvironmentadjust.cpp`)
- `LLFloaterEnvironmentAdjustAdvanced` (`llfloaterenvironmentadjustadvanced.cpp`)
- `LLPanelSettingsSkySunMoonTab` (`llpaneleditsky.cpp`)

## Proven control flow

Each implementation keeps a stable `LLSettingsSky::ptr_t`, converts slider degrees
to a quaternion in the commit callback, calls `setSunRotation()` or
`setMoonRotation()`, and then calls `update()` on that same sky object.

Slider values are refreshed when the editor opens, when its source sky is replaced,
or when the companion trackball changes. The working implementations do not poll
and rewrite slider values from `draw()`.

Personal Lighting creates a fixed local sky when necessary. A local day cycle is
frozen at its current frame; without a local environment, the effective parcel sky
is cloned. The local environment is then selected immediately.

## AyaneStorm implementation

`ASPanelCelestialPosition` follows the same stable-sky pattern. It refreshes when
the panel becomes visible and when the local environment is replaced. This keeps
tabs synchronized without racing slider drag/commit handling.

Body detection must use `findChildView()`, not `getChild<T>()`. The latter creates
a dummy widget when the requested name is missing, which caused Moon panels to be
misidentified as Sun panels and left the real Moon sliders without callbacks.

The panel's environment version marker must not equal
`LLEnvironment::NO_VERSION` (`-3`). Quick Preferences applies presets with that
default version. Treating `-3` as a self-update caused the replacement event to be
ignored and left the panel holding a stale sky object. `-4` is also reserved for
environment cleanup, so AyaneStorm uses `-5` for its local-sky creation marker.
