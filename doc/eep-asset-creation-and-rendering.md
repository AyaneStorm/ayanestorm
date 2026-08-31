# EEP asset creation, serialization, and rendering

## What an EEP is

EEP (Environmental Enhancement Project) environments are data assets, not
user-supplied GLSL shaders. The viewer supports three settings subtypes:

- sky;
- water;
- day cycle.

They are represented by `LLSettingsSky`, `LLSettingsWater`, and
`LLSettingsDay`. Their inventory subtype is stored in the settings inventory
item flags; the underlying asset type is `LLAssetType::AT_SETTINGS`.

## Authoring and saving

The viewer's environment editors modify an in-memory settings object. A sky
object contains scalar, vector, quaternion, and texture-UUID fields such as:

- sunlight and ambient colours;
- sun and moon rotations, scale, brightness, and texture UUIDs;
- cloud colour, density/position, scale, scroll, shadow, variance, and texture;
- legacy blue-density, blue-horizon, haze-density, haze-horizon, glow, gamma,
  density multiplier, and distance multiplier;
- planet/atmosphere radii and Rayleigh, Mie, and absorption density profiles;
- Mie anisotropy;
- moisture, droplet radius, ice level, rainbow texture, and halo texture;
- reflection-probe ambiance.

`LLSettingsSky::validationList()` defines required fields, types, ranges, and
defaults. The physical-profile defaults model an Earth-like atmosphere:
Rayleigh scale height 8 km, Mie scale height 1.2 km with anisotropy 0.8, and a
two-layer absorption/ozone profile.

On Save or Save As, the viewer creates or updates an inventory settings item.
It obtains the settings map, serializes it as LLSD notation, and uploads the
buffer through the simulator's `UpdateSettingsAgentInventory` capability as an
`AT_SETTINGS` asset. The inventory item points to the resulting asset UUID and
carries ordinary inventory permissions.

Relevant code:

- `indra/newview/llfloatereditenvironmentbase.cpp:244-329`;
- `indra/newview/llsettingsvo.cpp:118-263`;
- `indra/llinventory/llsettingssky.cpp:727-949`.

The viewer can also export the same settings map to disk in an LLSD format, but
the normal shared EEP is an inventory/server asset rather than a preset XML in
the installation tree.

## Day-cycle structure

A day-cycle asset is a container holding its sky and water frames inside the
same serialized LLSD asset. It has five tracks:

- track 0: water;
- tracks 1 through 4: sky at increasing altitude bands.

Each track is an array of normalized time positions in `[0,1]` and frame-name
references. A separate `frames` map contains the full embedded sky or water
settings for each referenced frame. Identical frames are deduplicated by a
settings hash when serialized. During playback, settings between keyframes are
interpolated; rotations use spherical interpolation where specified.

Relevant code: `indra/llinventory/llsettingsdaycycle.cpp:99-284`.

## How the asset becomes an image

Loading deserializes the asset's LLSD and constructs the appropriate settings
class. The active environment may come from a local override, parcel, region,
estate, or day-cycle track. The viewer then blends/selects the current sky and
water settings.

The values are consumed by viewer-owned rendering code. In this branch, the
legacy/EEP sky path caches blue density, haze, sunlight, ambient light, glow,
cloud shadow, atmospheric transmittance, and related values in `LLVOSky`.
`class1/deferred/skyV.glsl` calculates directional sunlight extinction and
haze/glow over the sky dome. `skyF.glsl` adds the rainbow and ice-halo textures.
Other passes render clouds, the sun/moon billboards, atmospheric haze on scene
geometry, and water.

Consequently, an EEP creator selects parameters and texture assets; they do not
package a replacement atmospheric shader. Every compatible viewer interprets
the same settings through its own renderer, so small visual differences between
viewer versions and graphics modes are expected.

## Relationship to AyaneStorm's procedural sunset

AyaneStorm's procedural sunset is viewer-local renderer functionality layered
on top of the active EEP. Its enable state and controls are not fields in the
EEP asset. It reads the EEP's live sun direction, scale, sunlight colour, and
sun texture, then supplies an analytic fallback disc and halo. Therefore it can
improve presentation locally, but saving an EEP does not transmit that feature
or its settings to another viewer.
