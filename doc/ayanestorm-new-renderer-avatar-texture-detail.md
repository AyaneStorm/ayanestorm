# Avatar detail textures and close-up refinement

Date: 2026-11-11  
Author: chanayane@firestorm

## Distance policy for eyes and small attachments

The native renderer identifies system eyeballs from their body part IDs. It treats a
worn mesh or prim as a small attachment when its longest scale axis is at most 25 cm.
Attachment points and object names are unreliable labels for jewelry, nails, lashes,
and mesh eyes. A qualifying texture takes the most detailed allowance of all its
tracked uses. Terrain, a larger surface, or an unclassified use cancels this cap.

| Camera distance | Longest side at that distance |
| --- | ---: |
| 1 m | 512 px |
| 2 m | 256 px |
| 4 m | 128 px |
| 8 m and farther | 64 px |

The cap can rise to 1,024 or 2,048 pixels inside one metre. The asset resolution,
texture feedback, available discard levels and memory allowance still determine the
actual mip. Rigged attachment distance is estimated from the nearest point of the
wearer's animated bounds. System eye distance uses the eye joint. Close-up allowances
rise immediately, while a four-second hold delays a decrease when the camera pulls away.

The mip floor affects fetch requests and downsizing. If Firestorm returns a finer image
from cache or a bake, the native upload can use a scaled copy. The original decoded
data remains available to cache and avatar callbacks. Server bake fetching can require
the full image. Lipstick, makeup and lashes baked into a head texture share its
resolution and cannot be assigned independent mips. `TextureLoadFullRes` bypasses
the small-detail cap. A pending texture callback can still require a finer mip.

## Refinement during texture pressure

The texture steering threshold still freezes general mip upgrades. A resident image
with recent GPU feedback requesting at least 512 texels across can request the sharper
mip if the entire new image, including its mip chain, fits alongside the old image
under the hard texture allowance. This also applies to world objects that fill the
view. The upload checks available room again when the decoded image arrives. The
driver reserve and user limits still apply.

Compile-time checks cover the distance bands and conversion to discard levels. The
viewer run still needs to verify the visual result and texture memory savings.
