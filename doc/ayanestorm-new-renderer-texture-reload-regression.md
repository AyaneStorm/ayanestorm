# Texture reload and blank-image regression after the renderer changes

Date: 2026-11-10  
Author: chanayane@firestorm

The follow-up [Vulkan VRAM stress reproduction](ayanestorm-new-renderer-vram-stress-reproduction.md)
records the overcommit and recovery measurements and the bounded first-image policy.

## Runtime failure and evidence

The user reports continued severe lag and white textures flashing with the latest changes.
The implementation is not accepted as fixed. This investigation does not claim restored FPS
or a runtime-confirmed cure for the flashing.

The current `AyaneStorm.log` records, between 17:43:08 and 17:43:28 UTC:

- Shrink counter: 17,464 → 21,103, or 3,639 shrinks in 20 seconds.
- Texture creation counter: 24,124 → 27,652, or 3,528 creations.
- Texture memory: 5,196 → 5,160 MB. Repeated replacement is not making sustained progress.
- No tracked images lost or released; white-slot face count stays 279. This does not
  rule out blank images being published behind an existing nonzero texture slot.
- Feedback: 9 then 11 completed windows per approximately 20-second reporting interval.
  A window needs at least 16 GPU frames. These intervals are longer than one second.

Later readings show a higher feedback rate and lower GPU time, while resizing continues.
The timeout defect therefore cannot be asserted to explain every replacement or all lag.

## Confirmed feedback-policy defect

`judgeTexture()` ignored GPU sizing when no window arrived in the preceding second.
At low frame rates that expires valid feedback before the next complete window can arrive.
The same texture alternates between GPU demand and face demand, bypassing the existing
four-second level/visibility hold.

The correction uses the latest completed feedback-window clock to age a texture's sample.
Waiting without a new window is not negative visibility evidence. Completed windows that
miss the texture still expire its demand after the existing four-second hold. Never-sampled
and imageless textures keep their separate first-image bootstrap path.

The original four-second level hold remains; no shader feedback generation, camera,
deformation ordering or geometry-residency policy is changed in this correction.

## Confirmed unsafe replacement-failure paths

### Invalid shrink source could publish a cleared image

Both backends trusted the queued CPU-relative source mip. If the copy condition failed,
creation fell through to clearing the replacement, and a following publish could display it.
This is a concrete blank-publication path, not proof that it caused every observed flash.

Example: CPU metadata says 2048 after a failed grow, but the published GPU image is still
1024 with 11 mips. A request to shrink to 256 carries relative mip 3 and needs 9 destination
mips. The previous check rejects `3 + 9 > 11`, clears the new image, then publishes it.
The valid copy is actually resident mip 2, with `2 + 9 == 11`.

The correction stores metadata for successfully created images and selects the source mip
from the resident dimensions, format, swizzle and available mip chain. If no valid copy
exists, no replacement is created or published; its queued memory reservation is released.
If the resident image already matches the requested size/mips, it remains in place.

### Failed grow could overwrite the displayed image

After a replacement allocation failed, the old `textureFor()` fallback returned the displayed
image to subsequent uploads and mip-generation commands. Those commands belonged to the
failed replacement and could have incompatible dimensions or layouts.

Shared `TextureReplacementState` now blocks upload, mip generation and publication for a
failed replacement until a new create transaction or destruction resets that slot. Sampling
continues using the published image directly; the failure flag must not make it disappear.
Vulkan and Metal use the same state. This preserves the previous image, which may remain
coarser until a subsequent valid replacement succeeds; it is not allocation-failure retry/ack
support for the producer's image metadata.

## Scope and diagnostics

Policy is isolated in `indra/asrenderer/asrtexturestreaming.h`.
Backend integration is in `asrdevicevulkan.cpp` and `asrdevicemetal.mm`; viewer demand uses
the completed-window policy in `asrworld.cpp`.

The texture-feedback log now includes response age and cumulative mip copies rebased/rejected.
These counters can establish whether the actual-resident-image recovery path is exercised.
No upstream texture-fetch changes or additional shader optimizations are included here.

## Validation status

### First run after the feedback and replacement correction

The viewer launched at 21:27:37 UTC on 2026-11-10. The new feedback-age and
rebased/rejected-copy diagnostics are present, confirming that this run includes the
correction. At 21:29:16 UTC, while meshes were still arriving:

- GPU passes total 4.77 ms; clusters 0.14 ms, groups 0.13 ms, vertices 0.37 ms,
  early draw 1.12 ms, and Mac OIT keys/tail/colour/resolve 0.35/0.27/0.40/0.14 ms.
- Driver usage is 9,103 of 10,343 MB; tracked textures use 5,018 MB.
- Geometry uses 2,860 MB; 1,081 meshes still have no geometry.
- Shrinks rose from 1,049 to 1,789 in twenty seconds while loading continued.
- Rebased/rejected mip copies remain 0/0. This sample does not establish that the
  allocation-failure recovery paths were needed or exercised.

These are loading samples, not an equivalent-camera performance comparison or confirmation
that flashing has stopped. Sustained memory use, replacement activity and user observations
are still needed before judging the correction.

### Club slowdown after the flashing stopped

The user confirms flashing stopped, but reports about four FPS within a minute in the club.
At 21:29:36 UTC the same run records:

- GPU time 102.81 ms, including clusters 24.25 ms, groups 11.41 ms, vertices 16.58 ms
  and early drawing 26.57 ms. This is not primarily the earlier transparency bottleneck.
- Driver usage 10,292 of 10,590 MB; allocator holds 10,140 MB, with 9,330 MB allocated.
- Tracked total 10,067 MB, including 5,698 MB textures. Texture allowance is 5,691 MB.
- 3,872 shrinks in twenty seconds; the worst frame waits 838.53 ms for the GPU and
  records 398 texture creates. These figures are consistent with residency pressure,
  although proving individual page faults requires a GPU residency trace.

## Pending correction: queued reservations and driver headroom

`getTextureMemory()` computed driver affordability as tracked texture use plus driver
free room. Tracked use includes queued texture reservations, while the driver reading
reflects allocations that already exist. Increasing queued reservations therefore raised
the driver-derived allowance; queued loads were not charged against that allowance.
The category limit could still constrain them, but it did not charge allocator overhead.

The corrected driver bound subtracts all tracked nontexture allocations, allocator/driver
overhead (`driverUsage - allocatorUsed` when positive), and ten percent driver headroom
from the driver budget. Queued textures consume this fixed remainder instead of enlarging
it. The existing category limit still applies. Without Vulkan allocator measurements,
the fallback uses the tracked total to estimate overhead and still reserves headroom.

Using the rounded 21:29:36 readings gives a driver-bound texture allowance near 4,200 MB,
instead of 5,691 MB. This is a deliberate reduction in residency, not a claim of restored FPS.
The patch changes `asrviewer.cpp` and shared policy only: no shader, mip-copy transaction,
system-body scheduling or upstream fetch changes are included.

Compile-time checks cover overhead, queued textures not increasing their own allowance,
queued geometry reducing it, budget excess, saturating subtraction and the non-VMA fallback.
Portable C++17 syntax checks pass with the existing unused `seconds` warning. The new
allowance has not been built or runtime-tested by the user.

### Build and source checks

The first Windows build failed with C2445 in the feedback-age diagnostic: subtracting
from `gFrameTimeSeconds` returns an implicit seconds unit, making the conditional against
`-1.f` ambiguous. The diagnostic now reads `.value()` before subtraction so both branches
are floats. This changes logging only. The user subsequently reported the build finished;
the fresh viewer log above confirms execution of the corrected diagnostic.

C++17 compile-time regression checks cover retained demand while waiting for slow feedback,
expiry after completed negative windows, recovering the correct resident mip after a failed
grow, invalid extents/mip chains, and isolation/reset of failed replacement transactions.
These assertions are evaluated by syntax-only compilation without building an executable.
Portable test-source syntax checks also cover the existing renderer checks.

The agent has not built the project. The user's Windows build and initial Vulkan run are
recorded above; Metal compilation and runtime behaviour remain unverified. The actual visual
result and sustained performance remain unverified. The broader renderer slowdown remains
an open investigation.
