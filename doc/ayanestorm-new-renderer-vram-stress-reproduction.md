# Vulkan VRAM pressure reproduction and texture admission

Date: 2026-11-10  
Author: chanayane@firestorm

## Standalone workload

The Windows standalone test renders 80 animated skeletons with 16 distinct rigged mesh
allocations each. The camera and mesh workload stay fixed while it queues hundreds of
mipmapped textures. It then replaces the images with 128-pixel versions, waits for the old
allocations to retire, and measures steady frame rate. The test uses the same renderer and
texture queues as the viewer. It does not reproduce viewer asset fetch, bake callbacks, or
network timing.

Run separately on the same GPU with the viewer closed:

```powershell
& 'E:\dev\AyaneStorm\Vulkan\build-asr\test\Release\asrenderer_test.exe' --vram-stress --seconds 5 --size 1280 720
& 'E:\dev\AyaneStorm\Vulkan\build-asr\test\Release\asrenderer_test.exe' --vram-managed --seconds 5 --size 1280 720
```

## RTX 3080 Ti results

| Mode | Baseline FPS | FPS with textures loaded | Driver usage with textures loaded | Driver usage after discard | FPS after retirement |
|---|---:|---:|---:|---:|---:|
| Forced overcommit | 123.2 | 3.0 | 12,468 MiB | 2,027 MiB | 3.3 over the first five seconds |
| Bounded first images | 120.2 | 120.3 | 6,827 MiB | 2,018 MiB | 120.0 |

An extended forced-overcommit run dropped from 117.6 to 2.6 FPS. Old texture allocations
retired in 3.21 seconds, reducing driver usage from 12,468 to 2,022 MiB. The next five
seconds averaged 2.9 FPS, with 356 ms of GPU wait on the last frame. During recovery the
GPU reported 100% utilization, 1,905 MHz graphics clock, and 65.7 °C. Over the following
20 seconds the run averaged 90.8 FPS and GPU wait returned to 7.73 ms. After the remaining
textures were destroyed, it averaged 117.5 FPS. The recovery lag outlasted allocation
retirement. Its underlying driver mechanism remains unconfirmed.

## Viewer changes awaiting a club run

The viewer uses the shared `firstTextureMip()` policy to size initial GPU uploads before
allocation. It keeps the original decoded image for bake callbacks and cache handling.
Queued allocations consume the texture allowance, which respects the configured viewer
limit and reserves driver headroom. Refinements pause at the steering target while small
first images continue to load. The patch also gives other avatars' bakes the native discard
policy and uses the wearer's rendered position to prioritize worn textures.

The managed Vulkan stress test and standalone self-tests passed. The Vulkan scene audit
found zero incorrect skinned positions in 815,457 samples and no occlusion mismatch.
MSVC syntax checks passed for `asrviewer.cpp`, `asrworld.cpp`, `llviewertexture.cpp`,
`llviewertexturelist.cpp`, and `asrbridge.cpp`. The club run still needs to verify avatar
loading, texture quality, and sustained frame rate with the integrated viewer.

## Shared VRAM headroom and existing settings

The renderer now sets a working budget below the driver allowance before splitting it
among allocation categories. It leaves 10% or 512 MiB, whichever is larger, with the
reserve limited to half the allowance on small budgets. It also deducts reported driver
and allocator overhead. Regular reservations check the total alongside their category
limit. The texture allowance falls as geometry grows, even below its original category
share. Ordinary fetched textures defer first uploads when even the smallest admissible
mip exceeds that allowance. Memory pressure also shortens unused mesh LOD retention.

`ASRendererVRAMBudgetMB` lowers the total working budget and takes effect on restart.
`RenderMaxVRAMBudget` remains the separate texture ceiling when
`FSLimitTextureVRAMUsage` is enabled. Both limits can reduce the working allowance.
Required scene buffer growth, boosted textures, and temporary texture replacements
retain their existing exceptional allocation paths. A sudden external allocation can
still lower the driver budget before existing resources retire.

The FPS results above predate this shared budget change. The new policy is covered by
compile-time headroom and cap checks and a standalone budget regression case for a
falling driver allowance. A new GPU run is needed for these latest changes.

## Viewer runtime confirmation before the shared budget change

On 2026-11-10, the Windows viewer executable was linked at 23:30:51 CET. The
shared budget source changes began at 23:31:04 CET. Vulkan ran from 23:31:35 to
23:46:37 CET with the earlier bounded first-image policy. The user reported no
slowdown during the session.

Across 42 sampled memory readings, driver usage remained below its allowance.
Peak usage was 8,596 of 9,894 MiB, leaving 1,298 MiB at the closest point.
The GPU timing samples had a 6.39 ms median and an 8.62 ms maximum. After the
startup minute, complete minute summaries ranged from about 135 to 253 FPS.
The renderer stopped with zero validation errors. The previous viewer log recorded
9 over-budget readings out of 14 and a maximum sampled GPU time of 175.74 ms.

Replaying the 42 memory samples against the initial 20% shared reserve put tracked
usage above the new budget in 38 samples. With the earlier 10% reserve, all 42 fit,
with approximately 171 MiB remaining inside the working budget at the closest point.
The shared policy now uses that margin and leaves out the additional texture reserve
inside the already reduced budget. The existing texture steering target still leaves
room for temporary uploads. This comparison uses logged samples and awaits runtime
verification of the shared policy.

## 23:50:59 CET build and close-up texture detail

The Windows executable linked at 23:50:59 CET includes the shared budget changes
made after the 23:30:51 CET baseline. The source timestamps and change history
identify these changes between the two builds:

- `asrdevice.h` and `asrmemorybudget.h` apply the existing 10% driver margin and
  allocator overhead to the entire working budget. The margin has a 512 MiB
  minimum, limited to half the allowance on small budgets. The native total cap
  still applies.
- `asrmemorybudget.cpp` checks the total when making regular reservations, removes
  the texture allowance floor and the additional 5% texture reserve, and guards
  borrowing and staging calculations against unsigned underflow.
- `asrviewer.cpp` uses the shared texture allowance, with the same configured
  texture limit. The texture-only driver calculation was removed from
  `asrtexturestreaming.h` and the standalone test now uses the shared allowance.
- `llviewertexture.cpp` checks whether a first upload fits after choosing its mip.
  The existing sharper-mip request freeze and its 85% steering threshold remain.
- `asrworld.cpp` uses the tracked working budget to trigger shorter unused mesh LOD
  retention. Texture demand from screen coverage and GPU feedback is unchanged.

All 11 driver memory readings stayed below the driver allowance. The previous
formula reproduces the new logged texture allowance to within 1 MiB in all 11
samples, using the new run's own readings and allowing for log rounding. Thus the
shared-budget move alone does not explain the close-up report. Texture refinement
was frozen in 4 of those 11 samples, versus 2 of the first 11 in the baseline.
At 23:53:33 CET, 3,399 MiB of textures exceeded the roughly 3,230 MiB steering
target (85% of 3,800 MiB), while remaining below the hard allowance. The existing
freeze clamps sharper requests even for close objects. The log does not identify
the particular object or measure its texture promotion latency.

The first build overlapped editing, so the Vulkan startup line is the independent
check for the shared budget. The 23:30 build reported the full 11,316 MiB driver
allowance at startup. The 23:50 build reported 10,180 MiB, consistent with applying
the shared reserve and overhead. Its two complete post-startup minutes averaged
161 and 182 FPS, compared with 136 and 141 FPS in the first two such minutes of
the earlier run. Both stayed below the driver budget. The different scenes and
run lengths prevent a controlled speed comparison.

## Crowd slowdown before the avatar detail texture changes

The next run used the same 23:50:59 CET executable and ran from 00:21 to 00:35 CET
on 2026-11-11. The user reported more people and renewed slowdowns. In the sustained
slow period, the main change in the samples was how many avatars had live rendering
data, including those kept ready near the camera view.

| CET | Skeletons / parked | Deformed positions | Sampled GPU time | Driver usage / allowance |
| --- | ---: | ---: | ---: | ---: |
| 00:31:49 | 74 / 42 | 256 MiB | 4.37 ms | 9,047 / 10,205 MiB |
| 00:32:29 | 73 / 11 | 610 MiB | 42.13 ms | 9,390 / 10,223 MiB |
| 00:33:29 | 74 / 13 | 610 MiB | 45.41 ms | 9,326 / 10,035 MiB |
| 00:34:29 | 73 / 40 | 290 MiB | 6.48 ms | 9,038 / 10,183 MiB |

At 00:33:29, tracked allocations exceeded the renderer's working budget by 421 MiB,
although the driver's reported allowance still had 709 MiB free. Skinning, the early
draw, and transparent rendering all slowed. One complete minute averaged 24.6 FPS.
Two system-body parts were shown in each of the samples above. The log establishes
increased rendering work and tighter memory headroom. It does not distinguish the
contribution of driver residency from the cost of the heavier scene. The later
small-attachment texture policy was absent from this run.

## Later Vulkan session and off-screen avatar retention

`AyaneStorm.old` contains the 00:37–01:18 CET Vulkan session from the 00:36:24
CET executable. At 00:38:22, all 64 skeletons were resident, the deformation
table held 493 MiB, and the GPU sample was 13.79 ms. At 00:40:22, 42 of 69
skeletons were parked, the table held 107 MiB, and the GPU sample was 3.14 ms.
Tracked VRAM exceeded the working budget at both times (7,856 of 7,733 MiB
and 7,754 of 7,547 MiB). These samples describe different camera views and
cannot isolate the cost of retaining avatars.

The viewer now uses a 12° prewarm margin and 0.6-second off-screen hold at
95% of its working budget. After three seconds below 90%, it restores the
45° margin and three-second hold. Visible avatars and the local avatar
remain eligible for immediate skinning. Parking releases derived skinning
data while retaining source mesh geometry and textures.

`asrenderer_test --avatar-sweep` repeats the same two-second camera turn
with an 80-avatar animated crowd at simulated 70% and 92% budget usage.
The test uses real renderer allocations and checks that the tighter policy
reduces both live skinning blocks and deformation table capacity, and that
turning back restores the bindings from resident geometry. It reports the
slowest return frame, frame rate, and driver usage. This mode does not fill
VRAM with extra textures.
