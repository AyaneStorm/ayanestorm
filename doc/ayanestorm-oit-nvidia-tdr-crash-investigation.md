# Exact OIT: NVIDIA driver fastfail (TDR) crash on RTX 40/50, not on RTX 3080 Ti

Author: chanayane@firestorm. Date: 2026-09-08.
Companion to `ayanestorm-oit-performance-audit-plan.md` (E3, E7, E11) and
`ayanestorm-oit-e1-e4-growth-race.md` (same failure class: garbage links).

## Symptom (field reports)

- Viewer freezes 1-2 s after entering a populated region with Exact OIT on;
  Windows reports `nvoglv64.dll`, `0xc0000409`, "TDR code 7".
- WinDbg on the user dump: `FAIL_FAST_FATAL_APP_EXIT` (fastfail subcode 7,
  **not** the stack-cookie subcode 2) raised inside `nvoglv64.dll`, stack
  unwalkable past the driver. This is the driver terminating the process
  after the GL context was lost to a TDR.
- Before the crash, alpha-blended hair renders wrong: near-white, blooming,
  with dark speckles (see the user's video frames), and one earlier report of
  solid magenta patches (the composite's "impossible" fallback branch).
- Predecessor build with the bounded fence wait logged
  `Exact OIT capture fence did not signal after 5s` and then stopped: the GPU
  was still executing the *previous* frame's composite when this frame's
  capture fence was queued behind it.
- Reproduces on RTX 5070 Ti (desktop i7-12700 box and a laptop), RTX 4090,
  driver 616.64. Does not reproduce on the developer's desktop RTX 3080 Ti in
  a quiet region.
- Last known-good builds (`b1c72f7f`, `bbd739c3`) predate E7 (wave-level
  allocation), E11 (`glClearTexImage` clears) and E3 (blend-uniform cache).

## Mechanism

A TDR is "one GPU packet > 2 s". The composite is one fullscreen draw whose
per-pixel work is `next`-chasing through `oitNodes`. If two pixels' lists
share a node (duplicate index) or a stale head points into a node the new
frame has overwritten, pass 1's in-place `next` rewrites cross-link the lists
into cycles; the traversal then runs for millions of iterations in a single
draw, the driver's watchdog fires, and the context is lost. The visible
corruption in the frames before is the same corrupted structure read as
colour/order. So the question is only what corrupts the lists.

## Defects found and fixed (all in the range since the known-good build)

1. **E11 dropped the clear's synchronisation** (`asexactoit.cpp`,
   `prepareCaptureBuffers()`). The head/count images are written by shader
   image stores (sort pass 1 `imageStore`), and nothing waits for that pass on
   the CPU. OpenGL 4.6 §7.12.2: `glClearTexImage` is ordered after shader
   image writes only with `GL_TEXTURE_UPDATE_BARRIER_BIT`; FBO clears need
   `GL_FRAMEBUFFER_BARRIER_BIT`. The pre-E11 FBO clear was implicitly
   synchronised by its FBO bind; E11 replaced it with `glClearTexImage` and no
   barrier. Whether the race materialises depends on the driver's clear path,
   which is the kind of thing that differs between GPU generations. Fix:
   `glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT)`
   before the clears, once per frame.

2. **E7's allocator election assumed helpers had left the wave**
   (`asExactOITReserveSubgroupF.glsl`). `subgroupElect()` picks the lowest
   *active* lane; atomics from helper invocations are dropped by hardware. If
   a returned helper is still active for subgroup ops (implementation-defined),
   the elected helper's `atomicAdd` is dropped, `base` broadcasts as 0 and
   every lane gets `0 + rank`: duplicate indices across all waves. Hair is the
   most helper-heavy geometry there is. Fix: fold `!gl_HelperInvocation` into
   `need`, have the lowest *needing* lane (`subgroupBallotFindLSB`) do the
   `atomicAdd`, publish with `subgroupMax()` over zero-initialised lanes; same
   rule for the `oitPad` `atomicMax`. Correct whether or not helpers are still
   in the wave, same cost. The earlier fix (every non-helper lane must reach
   `exact_oit_wave_max_pad()`, in all three capture-family shaders) stands.

3. **Captured emissive draws left the emissive program bound**
   (`asexactoit.cpp`, `handleCapturedEmissives()`; present since the OIT
   dispatcher was introduced, scene-dependent). `renderAlpha()` skips its
   vanilla emissive block, including `lastShader->bind()`, when the OIT path
   returns true, while its local `current_shader` still names the alpha
   program. The next group with the same target is drawn through the emissive
   capture shader with no rebind: fragments become glow-only nodes (white,
   blooming hair after any glowing attachment), texture/uniform setup lands on
   the wrong program, skinned draws use the skinned emissive program's last
   matrix palette. This is why it needs a glowing alpha object in the same
   pass to show, and why a quiet-region test never sees it. Fix: restore the
   previously bound program after the emissive draws, exactly as vanilla does.
   `ASAVBOIT::handleCapturedEmissives()` has the same shape and should get the
   same restore.

4. Fence-timeout fallback now switches `ASRenderOITMode` to Standard: the
   dispatcher re-derives `ASRenderExactOIT` from the mode every frame, so
   clearing only the boolean was undone one frame later.

Earlier in this investigation (already committed as `1edc54d48f` or in the
same working tree): bounded `glClientWaitSync` retry in `waitValidation()`;
every list traversal in `asExactOITCompositeF.glsl` capped at
`oitNodeCapacity` (note: this bounds a cycle at tens of millions of iterations
per pixel, which still exceeds the TDR budget — it prevents a true infinite
loop, not the crash; only preventing corruption does that); the composite's
magenta "impossible" branch now degrades to `blend_shallow()`; `#version 450`
limited to the four `*Subgroup*` files; `mHasShaderSubgroup` additionally
requires `GL_FRAGMENT_SHADER_BIT` in `GL_SUBGROUP_SUPPORTED_STAGES_KHR`;
`ASRenderExactOIT` default set to 0 so a fresh profile's `-1` migration lands
on Standard.

## Verify

Populated region with glowing attachments and hair, Exact OIT on, RTX 40/50:
hair keeps its colour, no bloom halo, no crash. Debug mode 4 (sorted order)
all green; debug mode 6 shows no overflow. `ASRenderExactOITSubgroup=false`
remains the A/B switch for the E7 path.
