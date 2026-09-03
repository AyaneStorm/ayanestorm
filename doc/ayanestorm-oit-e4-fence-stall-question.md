# Exact OIT E4 (fence-overlapped readback): severe regression, root cause unclear

Author: chanayane@firestorm (question for a higher-end reviewing model).
Date: 2026-09-03.
Companion to `ayanestorm-oit-performance-audit-plan.md`, item E4 (section
"E4. Non-blocking readback: fence + persistent map, overlap with sort pass
1"). That plan was written by a higher-tier model and is being implemented
in commits on top of `b49aefce07` ("exact OIT phase 0 + 1"), which is the
last known-good state (measured ~29 FPS in the reference scene below).

## Status: E4 implemented as specified, causes a severe regression under load. Two diagnostic builds ruled out one hypothesis. Need a diagnosis before continuing.

## Reference scene

Third-person camera, avatar wearing a sheer outfit with a long ponytail
(detailed alpha hair), standing near water with active splash particle
sprites in view (heavy overdraw, many small alpha-blended sprites, some
additive/glow). `RenderOITMode` = Exact OIT throughout.

GPU: NVIDIA GeForce RTX 3080 Ti. Driver: NVIDIA 596.49. `GL_VERSION`:
4.6.0. (From `AyaneStorm.log`, `LLGLManager::printGLInfoString`.)

Two test spots used throughout:
- **"Empty" spot**: same general area, looking at plain ground/terrain,
  ~no transparent geometry in view (`Nodes used 0`).
- **"Sprite" spot**: facing the water splashes + avatar, as described above.

## Baseline (pre-E4, commit b49aefce07)

- Sprite spot: **~29 FPS** (this was the user's original complaint — "laggy"
  — but still an order of magnitude faster than what E4 produced).
- Empty spot, Exact OIT enabled: **31 FPS**. Same spot, Exact OIT disabled
  (`RenderOITMode` = Standard): **40 FPS**. This ~9 FPS / ~29% gap with
  *zero* transparent geometry in view was the motivating measurement for
  implementing E4: `Nodes used 0`, so it isolated a fixed per-frame cost
  (not sorting, not overdraw) — pointing at the plan's finding #1, the
  synchronous `glGetBufferSubData` readback stall described below.

## What E4 asks for (plan text, abbreviated)

> `validateCapture()` calls `glGetBufferSubData` which waits for every
> command that touches the buffer (in practice, for everything queued) and
> blocks the CPU. This removes CPU/GPU overlap for the rest of the frame.
>
> Design:
> ```
> capture draws
> fence = glFenceSync()
> issue sort pass 1 (safe to run even on an overflowed capture)
> glClientWaitSync(fence)               <- waits only until capture is done
> read control from persistent mapping  <- no driver copy, no extra sync
> if overflow: vanilla fallback (sort results are simply ignored)
> else: issue remaining passes (count from control[3]) + composite
> ```
>
> Pass 1 runs before the overflow decision. An overflowed capture has next
> pointers only to successfully allocated nodes (allocation failure returns
> before linking), so pass 1 cannot read out of bounds. The fallback ignores
> all list data.

The full original E4 write-up (control buffer persistent mapping, fence
placement, `waitValidation()` split, trap list) is in
`ayanestorm-oit-performance-audit-plan.md` under "E4." if more detail is
needed; the implementation below follows it closely.

## What was implemented

`fsexactoit.h`: added to `Resources`: `U32* controlMapped = nullptr;` and
`GLsync captureFence = 0;`. Split the old `validateCapture()` into
`captureInactive()` (cheap early-out check), `beginValidation()` (memory
barrier only), and `waitValidation()` (fence wait + control readback +
overflow policy). `composite()` gained a `bool sort_pass_1_issued` parameter.

`fsexactoit.cpp`, control buffer allocation (`allocateNodePool()`):

```cpp
glGenBuffers(1, &sResources.control);
glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.control);
const U32 control[4] = { 0, sResources.capacity, 0, 0 };
sResources.controlMapped = nullptr;
static LLCachedControl<bool> force_fallback(
    gSavedSettings, "RenderExactOITForceReadbackFallback", false);
if (!force_fallback && gGLManager.mGLVersion >= 4.39f && glBufferStorage && glMapBufferRange)
{
    const GLbitfield storage_flags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT |
        GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, sizeof(control), control, storage_flags);
    sResources.controlMapped = static_cast<U32*>(glMapBufferRange(
        GL_SHADER_STORAGE_BUFFER, 0, sizeof(control),
        GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT));
}
if (!sResources.controlMapped)
{
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(control), control, GL_DYNAMIC_DRAW);
}
glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
sResources.available = glGetError() == GL_NO_ERROR;
```

(`prepareCaptureBuffers()`, which resets the control words every frame via
`glBufferSubData`, is unchanged — still legal under `GL_DYNAMIC_STORAGE_BIT`.)

Fence creation, right after `markCaptureCompleted()` in
`renderPostDeferredCapture()`:

```cpp
markCaptureCompleted();
if (sResources.captureFence)
{
    glDeleteSync(sResources.captureFence);
}
sResources.captureFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
return true;
```

`waitValidation()` (this run's instrumented version; the `LL_INFOS` block
is new diagnostic code, not part of the original E4 patch):

```cpp
FSExactOIT::ValidationResult FSExactOIT::waitValidation(bool mouselook, U32& maximum_list)
{
    maximum_list = 0;
    static LLCachedControl<S32> debug_mode_wait(gSavedSettings, "RenderExactOITDebugMode", 0);
    if (sResources.captureFence)
    {
        LL_PROFILE_ZONE_NAMED("Exact OIT fence wait");
        const std::chrono::steady_clock::time_point wait_start = std::chrono::steady_clock::now();
        GLenum result;
        bool first_call = true;
        do
        {
            result = glClientWaitSync(sResources.captureFence,
                first_call ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, 1000000000ull);
            first_call = false;
        }
        while (result == GL_TIMEOUT_EXPIRED);
        glDeleteSync(sResources.captureFence);
        sResources.captureFence = 0;
        if (debug_mode_wait != 0)
        {
            const auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - wait_start).count();
            static U32 wait_frame_counter = 0;
            if (++wait_frame_counter >= 60 || wait_us > 2000)
            {
                wait_frame_counter = 0;
                LL_INFOS("ExactOIT") << "Fence wait took " << wait_us << " us, result "
                    << (result == GL_ALREADY_SIGNALED ? "ALREADY_SIGNALED" :
                        result == GL_CONDITION_SATISFIED ? "CONDITION_SATISFIED" : "OTHER")
                    << LL_ENDL;
            }
        }
    }

    U32 control[4] = {};
    if (sResources.controlMapped)
    {
        LL_PROFILE_ZONE_NAMED("Exact OIT mapped control read");
        memcpy(control, sResources.controlMapped, sizeof(control));
    }
    else
    {
        LL_PROFILE_ZONE_NAMED("Exact OIT validation readback");
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.control);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(control), control);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    maximum_list = control[3];
    recordCaptureStats(control[0], control[3], mouselook);
    sResources.lastRequiredNodes = control[0];
    if (captureOverflowed(control[0], control[2]))
    {
        return ValidationResult::FALLBACK_REQUIRED;
    }
    sResources.consecutiveOverflowsAtCap = 0;
    if (control[0] > sResources.capacity * 3 / 4 && sResources.capacity < U32(safeNodeCapacity()))
    {
        growNodePool(control[0] * 2u);
    }
    return ValidationResult::COMPLETE;
}
```

`finishFrame()`, the new speculative-issue ordering:

```cpp
void FSExactOIT::finishFrame(LLPipeline& pipeline, LLRenderTarget& screen,
                             LLVertexBuffer& screen_triangle, bool cube_snapshot,
                             bool impostor_render, bool mouselook)
{
    if (captureInactive(cube_snapshot, impostor_render))
    {
        return;
    }

    beginValidation();   // glMemoryBarrier(SHADER_STORAGE | SHADER_IMAGE_ACCESS)

    static LLCachedControl<bool> compute_sort_requested(gSavedSettings, "RenderExactOITComputeSort", false);
    const bool will_use_compute_sort = compute_sort_requested && sResources.computeSortAvailable;
    static LLCachedControl<bool> no_speculative(gSavedSettings, "RenderExactOITNoSpeculativeSort", false);
    const bool sort_pass_1_issued = !will_use_compute_sort && !no_speculative;
    if (sort_pass_1_issued)
    {
        bindCompositeResources();   // rebinds image units 0/1 and SSBO bindings 0/1
        issueSpeculativeFirstSortPass(screen_triangle);
    }

    U32 maximum_list = 0;
    const ValidationResult validation = waitValidation(mouselook, maximum_list);
    if (validation == ValidationResult::FALLBACK_REQUIRED)
    {
        VanillaFallbackScope fallback_scope;
        /* ... unchanged vanilla re-render of the alpha pool ... */
        return;
    }

    composite(screen, screen_triangle, maximum_list, sort_pass_1_issued);
    /* ... unchanged debug-alpha dispatch ... */
}
```

`issueSpeculativeFirstSortPass()` (new function, runs the equivalent of the
old loop's `width == 1` iteration):

```cpp
static void issueSpeculativeFirstSortPass(LLVertexBuffer& screen_triangle)
{
    static LLCachedControl<bool> opaque_cutoff(gSavedSettings, "RenderExactOITOpaqueCutoff", true);
    static LLStaticHashedString oit_pass("oitPass");
    static LLStaticHashedString oit_first_sort_pass("oitFirstSortPass");

    LL_PROFILE_GPU_ZONE("Exact OIT speculative sort pass");
    LLGLDepthTest depth(GL_FALSE);
    gGL.setColorMask(false, false);
    gExactOITCompositeProgram.bind();
    gExactOITCompositeProgram.uniform1i(oit_pass, 1);
    gExactOITCompositeProgram.uniform1i(oit_first_sort_pass, opaque_cutoff);
    screen_triangle.setBuffer();
    screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    gExactOITCompositeProgram.unbind();
    gGL.setColorMask(true, true);
}
```

`composite()` was changed to accept `sort_pass_1_issued` and start its own
sort loop from `width = 2` instead of `width = 1` when the speculative pass
already ran (verified in the shader that `oitPass == 1` never reads
`oitDebugMode`, only `oitFirstSortPass`, so the speculative call — which
does not set `oitDebugMode` — is a bit-identical stand-in regardless of the
active debug mode):

```cpp
used_compute_sort = !sort_pass_1_issued &&
    sortWithCompute(screen.getWidth(), screen.getHeight(), maximum_list);
if (!used_compute_sort)
{
    ...
    gExactOITCompositeProgram.uniform1i(oit_pass, 1);
    const U32 start_width = sort_pass_1_issued ? 2u : 1u;
    for (U32 width = start_width; width < maximum_list; width <<= 1)
    {
        ...
        screen_triangle.drawArrays(LLRender::TRIANGLES, 0, 3);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        ++sort_passes;
    }
}
```

The composite (pass 1) shader itself (`exactOITCompositeF.glsl`) was **not**
modified by E4 — `oitPass == 1` branch, unchanged:

```glsl
if (oitPass == 1)
{
    uint remaining_runs = imageLoad(oitListCounts, pixel).r;
    if (oitFirstSortPass != 0 && remaining_runs > 1u)
    {
        head = prune_behind_opaque_cutoff(head, remaining_runs);
        imageStore(oitListCounts, pixel, uvec4(remaining_runs, 0u, 0u, 0u));
        imageStore(oitHeadPointers, pixel, uvec4(head, 0u, 0u, 0u));
    }
    if (remaining_runs > 1u)
    {
        uint output_runs;
        head = natural_merge_pass(head, output_runs);
        imageStore(oitListCounts, pixel, uvec4(output_runs, 0u, 0u, 0u));
        imageStore(oitHeadPointers, pixel, uvec4(head, 0u, 0u, 0u));
    }
    frag_color = vec4(0.0);
    return;
}
```

`releaseResources(bool)` was extended to unmap the control buffer (if
mapped) and delete any pending fence before deleting the control buffer,
mirroring the allocation-side additions. Not reproduced here; believed
uncontroversial.

## Test results

All at the "sprite" spot unless noted, static camera, `RenderExactOITDebugMode`
nonzero (required for the instrumentation to log) unless noted.

1. **Empty spot**, Exact OIT on, E4 build: **32-33 FPS** (up from 31 FPS
   pre-E4 baseline; modest, expected gain, `Nodes used 0`).
2. **Sprite spot**, E4 build, default settings (`RenderExactOITForceReadbackFallback`
   = FALSE, i.e. persistent-coherent mapping active): **~1.3 FPS**.
   `debug mode 4` (sort-order validity) reports all green (no invalid
   order detected). Image looks visually correct. Log:
   ```
   Nodes used 1548809 / capacity 18273280, max pixel list 78, sort passes 7
   ```
   (`sort_passes` here already accounts for the speculative pass, i.e. total
   real pass count for `ceil(log2(78)) = 7` is correct, matching the
   pre-speculative-issue formula — no double-counting bug found.)
3. **Sprite spot**, same build, `RenderExactOITForceReadbackFallback` = TRUE
   (forces the plain `glGetBufferSubData` path instead of the persistent
   map, toggled live via Debug Settings, no rebuild): **still ~1-3 FPS**
   (user reported both "3 FPS" and, moments earlier at the same spot with
   the setting FALSE, "1.3 FPS"; treat this as "still catastrophically
   slow, same order of magnitude," not as a precise second data point —
   FPS was not re-measured carefully enough between the two toggle states
   to claim more precision than that). Fence-wait instrumentation log,
   both states, essentially identical:
   ```
   FALSE: Fence wait took 206927 us, result CONDITION_SATISFIED
   TRUE:  Fence wait took 223982 us, result CONDITION_SATISFIED
   ```
   This appears to **rule out persistent-coherent mapping** as the cause:
   the wait duration and FPS are both in the same catastrophic range
   (~1-3 FPS, ~210-220 ms fence wait) whether or not
   `GL_MAP_COHERENT_BIT`/`GL_MAP_PERSISTENT_BIT` is used.
   `GL_CONDITION_SATISFIED` (not `GL_TIMEOUT_EXPIRED`) means the wait is
   not hitting the 1-second timeout loop; it is a genuine ~220 ms wait for
   the fence to signal.
4. User confirmed explicitly: **the exact same sprite scene was much
   faster on pre-E4 code** (commit `b49aefce07`, i.e. the ~29 FPS baseline
   quoted above), so this is a regression introduced by this session's E4
   changes specifically, not a pre-existing cost newly exposed by more
   accurate measurement.

A third diagnostic (`RenderExactOITNoSpeculativeSort`, which would make
`sort_pass_1_issued` false so pass 1 runs entirely inside `composite()`
*after* `waitValidation()` returns, i.e. architecturally identical to
pre-E4 ordering except the readback mechanism itself, fence vs. blocking
`glGetBufferSubData`) has been added to the code but **not yet built or
tested** — the user asked to consult a higher-tier model before spending
another build cycle guessing.

## Hypotheses considered so far (unconfirmed)

- ~~Persistent-coherent mapping forces implicit driver-side sync under
  heavy atomic write contention~~ — ruled out by test 3 above (same result
  with plain `glGetBufferSubData`).
- Issuing the fullscreen pass-1 draw immediately after the capture draws
  (separated only by `glMemoryBarrier(SHADER_STORAGE | SHADER_IMAGE_ACCESS)`)
  causes the driver to serialize capture and pass-1 far worse than the old
  ordering did, e.g. because pass-1's scattered `oitNodes[]` reads directly
  follow ~1.5M nodes' worth of atomically-contended, cache-unfriendly
  writes from thousands of overlapping/additively-blended sprite
  fragments, and the *old* blocking `glGetBufferSubData` (which stalls
  until "everything queued" completes, per the plan's own finding #1)
  may have been acting as an inadvertent full pipeline drain that let the
  driver fully retire/coalesce that contended write traffic before any
  read-heavy pass touched the same memory — something the fence-based
  wait, being more "precise," no longer provides. This is plausible but
  unconfirmed; the `RenderExactOITNoSpeculativeSort` toggle (untested)
  would confirm or refute it directly: if forcing pass 1 to run strictly
  after the wait (same ordering as pre-E4, only the readback mechanism
  differs) restores ~29 FPS, this is confirmed.
- (Low-confidence, probably not it) `bindCompositeResources()` is called
  twice per frame now (once before the speculative pass, once again inside
  `composite()`, both just `glBindImageTexture`/`glBindBufferBase` calls,
  no barrier). This is cheap CPU-side redundancy, not plausibly a 10x+ GPU
  stall on its own, but is mentioned for completeness.
- A driver-specific behavior around `GL_SYNC_GPU_COMMANDS_COMPLETE` fences
  interacting badly with the specific sequence of atomic-heavy compute-like
  fragment shader work (the capture shaders' `atomicAdd`/`imageAtomicExchange`/
  `imageAtomicAdd` chain) immediately followed by a fence + a dependent
  draw — e.g. the driver may insert a much coarser sync point than
  `GL_SYNC_GPU_COMMANDS_COMPLETE` nominally requires, effectively
  serializing the GPU in a way that manifests as this large wait,
  specifically because of *what* precedes the fence (heavy atomic
  contention) rather than fence usage in general (the empty-scene case,
  `Nodes used 0`, saw no regression at all). Loosely, this is the same
  concern as the "old blocking readback was an inadvertent drain" hypothesis
  above, framed as a driver-implementation fact rather than an accident of
  the old code: if NVIDIA's driver requires draining the *entire* prior
  command stream (not just work touching the fenced buffer) before a fence
  created after heavy unordered image/SSBO atomic writes can signal, then
  no reordering of *our* code changes that cost — it would just move where
  the same stall is paid, and the apparent "regression" would actually be
  the true always-there cost that the old code paid at a different, less
  obviously-attributed point (a possibility the reference plan's finding #1
  and this doc's own baseline measurement did not anticipate or rule out).

## Downstream dependency: E5 and E6 are next in the plan

Not yet implemented, but relevant to your answer: the plan's Phase 2 is
E4, E5, E6 together, in that order, specifically because "E5 builds on E4's
restructuring" (both touch `composite()`/`finishFrame()`). E5 adds a
shallow-list (`count <= K`) fast path with an `OIT_SORTED` flag bit packed
into the same count image E4 already reads/writes; E6 adds chunked-run
sorting inside pass 1, the same pass this doc's speculative-issue change
wraps. If your recommended fix for E4 changes `composite()`'s signature,
the sort-pass loop structure, or how/when pass 1 is issued, please say
whether E5 and E6 as specified in `ayanestorm-oit-performance-audit-plan.md`
still apply unmodified on top of it, or whether they would need to change
too — so this session does not implement E5/E6 against a design your answer
already obsoletes.

## Note on how this will be used

This is a single-shot consultation, not a back-and-forth: there is no
follow-up round planned, so please make the answer self-sufficient rather
than diagnostic-only. Concretely that means: don't stop at naming the most
likely cause — give a concrete code-level fix or replacement design for E4
that this session can implement directly, ranked if you have more than one
candidate, each with a cheap way to tell (from FPS + the existing log
output shown above) whether it worked, so a failed first attempt still
tells us what to try next without writing another doc like this one.

## Questions

1. Given the ruled-out hypothesis and the untested `RenderExactOITNoSpeculativeSort`
   diagnostic, what is the most likely root cause of the ~200ms fence wait
   under heavy atomic contention, and is the speculative-pass-1 reordering
   itself the right thing to suspect, or is there a more fundamental problem
   with using `glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)` right after a
   heavily atomic-contended capture pass on typical desktop GL drivers
   (NVIDIA/AMD/Intel)? Note the apparent tension this doc has not resolved:
   FPS measurably dropped from ~29 to ~1-3 in the *same* scene after E4, a
   real user-visible change, not just a different profiler zone name — so
   whatever the root cause is, it must explain an actual new cost, not only
   a relabeling of a cost that was already being paid (and hidden inside)
   the old `glGetBufferSubData` stall.
2. On NVIDIA's Windows driver specifically (596.49, GL 4.6): is there a
   known interaction between `glFenceSync`/`glClientWaitSync` and heavy
   `imageAtomicExchange`/`imageAtomicAdd`-driven fragment shader traffic
   (thousands of overlapping/additive sprite fragments writing a shared
   linked-list structure) that would produce this kind of driver-side
   serialization specifically under high atomic contention but not under
   low contention (the empty-scene case showed no regression at all)?
3. Is the plan's E4 design (issue pass 1 speculatively, before knowing
   overflow status, purely to overlap it with the fence wait) fundamentally
   sound, or does the "safe to run even on an overflowed capture" reasoning
   have a hidden assumption that breaks down specifically for very large,
   heavily-contended node counts (~1.5M nodes, 78-deep lists) even though
   it is correct for smaller/lighter scenes?
4. Concretely, and this is the answer this session most needs: what should
   be implemented next? Please pick and fully specify one of, or propose a
   better fourth option than, the following — don't just name which
   category, give the actual code-level change:
   - (a) Keep the speculative pass-1 overlap but change *how* or *when* it
     is issued (e.g. a different barrier, a different fence type, splitting
     capture into smaller batches, or some other concrete restructuring)
     so it no longer collides with heavy atomic contention.
   - (b) Abandon the speculative-pass-1 overlap entirely and keep only the
     fence-based non-blocking readback (issue pass 1 strictly after
     `waitValidation()`, i.e. E4 minus its main claimed benefit but still
     avoiding the CPU-side `glGetBufferSubData` stall for the readback
     itself) — if you pick this, is the ~1-2 FPS gain seen in the empty
     scene (31 to 32-33) actually worth keeping, or is it noise/measurement
     error not worth the added complexity and risk versus just reverting?
   - (c) Revert E4 entirely back to the pre-E4 blocking `glGetBufferSubData`
     path (commit `b49aefce07`) and treat the plan's finding #1 (the fixed
     ~9 FPS / 29% cost with zero transparency in view) as something to
     solve a different way, or accept as an inherent cost of this
     architecture not worth chasing further — if the latter, say so plainly
     so this session stops trying.
   - (d) Something else this doc's authors did not consider.
5. If your answer depends on data this doc does not already contain (e.g.
   a Tracy capture, specific `gGLManager` capability flags, particle count,
   VRAM headroom, or anything else), you will not get a chance to ask for
   it afterward — so instead of requesting it, answer conditionally: state
   which answer applies under which value of the missing data, and tell
   this session how to determine which branch it is actually in using
   things already at hand (the log, in-viewer diagnostics, or a single cheap
   local check) without needing another round-trip to you.

---

# Answer (reviewing model, 2026-09-03)

## Root cause: the atomically-updated control SSBO was placed in host memory

The E4 code, exactly as the plan wrote it (so the mistake is the plan's), maps
the **control buffer itself** with `GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT |
GL_MAP_COHERENT_BIT`. On NVIDIA, and on discrete GPUs generally, a buffer
created with `MAP_READ | PERSISTENT | COHERENT` is allocated in **system
memory** so the CPU can read it coherently. But this buffer is the target of
`atomicAdd(oitNodeCount)`, `atomicMax(oitPad)` and `atomicOr(oitOverflow)`
from **every captured fragment**. Each of those atomics now crosses PCIe to
host memory instead of hitting L2. About 1.5 M nodes plus the rejected no-op
fragments, several atomics each, serialized on one address at PCIe latency,
is exactly a 200 ms capture.

This explains every observation:

- empty spot (`Nodes used 0`): no fragment atomics, no regression, small gain;
- sprite spot: cost proportional to fragment count, catastrophic;
- `Fence wait took ~210 ms, CONDITION_SATISFIED`: the wait is genuine, the GPU
  really needs 200 ms to finish the capture because the capture is now bound
  by PCIe atomics;
- debug mode 4 green: the data is right, only its location is wrong.

**Test 3 did not rule this out.** `RenderExactOITForceReadbackFallback` is a
`static LLCachedControl` read only inside `allocateNodePool()`. Toggling it
live changes nothing until the control buffer is reallocated, which needs a
`releaseResources()` + `allocateResources()` cycle (mode switch or resize).
The buffer stayed host-mapped during test 3, so both readings measured the
same configuration.

**Zero-build confirmation, do this first:** set
`RenderExactOITForceReadbackFallback` = TRUE, then switch `RenderOITMode` to
Standard and back to Exact OIT (this releases and reallocates the control
buffer through `captureEligible()`). Expected at the sprite spot: about
29 FPS or better and a fence wait of a few ms. If that holds, the diagnosis
is confirmed and the fix below is the whole job.

## Fix: keep the control SSBO device-local, copy 16 bytes into a separate host-visible readback buffer

The design (fence, speculative pass 1, persistent mapping for the *readback*)
is sound. Only the placement is wrong: shaders must never touch the mapped
buffer. Copy the four control words on the GPU with `glCopyBufferSubData`
from the device-local control buffer into a tiny persistently mapped readback
buffer, then fence.

`fsexactoit.h`, `Resources`: replace `U32* controlMapped` with
```cpp
GLuint readback = 0;          // 16-byte host-visible copy of the control words
U32*   readbackMapped = nullptr;
```

`allocateNodePool()`, control section (restore the pre-E4 control buffer and
add the readback buffer):
```cpp
glGenBuffers(1, &sResources.control);
glBindBuffer(GL_SHADER_STORAGE_BUFFER, sResources.control);
const U32 control[4] = { 0, sResources.capacity, 0, 0 };
// Device-local. Fragments hit this buffer with atomics; it must never be host-mapped.
glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(control), control, GL_DYNAMIC_DRAW);
glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

sResources.readback = 0;
sResources.readbackMapped = nullptr;
if (gGLManager.mGLVersion >= 4.39f && glBufferStorage && glMapBufferRange)
{
    const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glGenBuffers(1, &sResources.readback);
    glBindBuffer(GL_COPY_WRITE_BUFFER, sResources.readback);
    glBufferStorage(GL_COPY_WRITE_BUFFER, sizeof(control), nullptr, flags);
    sResources.readbackMapped = static_cast<U32*>(
        glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, sizeof(control), flags));
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    if (!sResources.readbackMapped)
    {
        glDeleteBuffers(1, &sResources.readback);
        sResources.readback = 0;
    }
}
sResources.available = glGetError() == GL_NO_ERROR;
```
Remove `RenderExactOITForceReadbackFallback` (it cannot work as a live toggle
and is no longer needed). The `glGetBufferSubData` path on `control` stays as
the fallback when `readbackMapped == nullptr`.

`renderPostDeferredCapture()`, after `markCaptureCompleted()`:
```cpp
if (sResources.readbackMapped)
{
    // Make the fragments' SSBO atomics visible to the copy, then copy the
    // four control words into the host-visible readback buffer. The copy is a
    // GPU command and executes in order after the capture draws.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_COPY_READ_BUFFER, sResources.control);
    glBindBuffer(GL_COPY_WRITE_BUFFER, sResources.readback);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, 4 * sizeof(U32));
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}
if (sResources.captureFence) glDeleteSync(sResources.captureFence);
sResources.captureFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
```
`GL_BUFFER_UPDATE_BARRIER_BIT` is the barrier the spec names for
`glCopyBufferSubData` reading shader-written data. Keep `beginValidation()`'s
`SHADER_STORAGE | SHADER_IMAGE_ACCESS` barrier unchanged for pass 1.

`waitValidation()`: unchanged except it reads `sResources.readbackMapped`
instead of `controlMapped`. One slot suffices: the CPU reads it before the
next frame's copy is issued, so there is no write-after-read hazard.

`releaseResources()`: unmap and delete `readback` (bind as
`GL_COPY_WRITE_BUFFER`, `glUnmapBuffer`, `glDeleteBuffers`). The control
buffer needs no unmapping any more.

Keep the speculative pass 1 and the `RenderExactOITNoSpeculativeSort`
diagnostic for one build, then delete that setting.

## Expected results and how to read them

| Measurement | Expected after fix | If not |
|---|---|---|
| Sprite spot FPS | at least the pre-E4 29 FPS, probably slightly more | see next rows |
| `Fence wait took` at sprite spot | a few ms at most (capture GPU time minus the CPU time spent issuing pass 1) | wait still near the frame time: the GPU is still slow before the fence; run the next row |
| Same scene with `RenderExactOITNoSpeculativeSort` = TRUE | no FPS change (the speculative pass is not the problem) | if FPS improves only with it TRUE, keep pass 1 after the wait (option b) and report that result |
| Empty spot | 32–33 FPS as already measured | — |

The 1–2 FPS gain at the empty spot is real but small. E4's value was never
the empty scene: the old stall grows with GPU frame time, the new one waits
only for the capture while the GPU already runs pass 1.

## Answers to the numbered questions

1. Root cause: host-memory atomics, above. The fence, the speculative pass
   and the reordering are not suspects; nothing in the GL sync model costs
   this much. The new cost is literally the atomics moving from VRAM to
   PCIe.
2. No fence/atomic interaction exists on NVIDIA. What exists is the
   allocation rule: `MAP_READ | PERSISTENT | COHERENT` buffers live in
   system memory, and GPU atomics to system memory are extremely slow. AMD
   and Intel drivers behave the same, so the fix is portable.
3. The design is sound and the "safe on overflow" reasoning holds at any
   node count: allocation failure returns before linking, so pass 1 can
   only follow links to written nodes. 1.5 M nodes and 78-deep lists change
   nothing.
4. Implement the fix above (option d). Do not revert and do not drop the
   speculative pass.
5. No missing data is needed. The zero-build confirmation decides the
   branch: if the mode-switch trick restores FPS, the fix applies; if it
   does not, test `RenderExactOITNoSpeculativeSort` = TRUE next (already in
   the code).

## E5 and E6 still apply unmodified

Neither depends on where the readback buffer lives. Two clarifications:

- With E5 the speculative pass 1 is still issued every frame; pixels with
  `count <= K` early-out inside it with one image load each. When the
  readback then says `maximum_list <= K`, `composite()` issues zero further
  passes. That is intended.
- With E6 compute the total pass count `P` from the E6 formula. The
  speculative pass is pass 1 of `P`; `composite()` issues `P - 1` more:
  `for (U32 i = 1; i < P; ++i) { draw; barrier; }`. That loop replaces the
  `start_width` logic.
