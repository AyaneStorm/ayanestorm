# Graphics Quality Switch Stall / Disconnect (Shader Reload)

Investigation of the 100+ s freeze and region disconnect when moving the
graphics quality slider (2026-10-26, Windows, NVIDIA, 12 GB VRAM).

## Symptom

- One quality slider change froze the main loop ~112 s (log 22:31:04Z to 22:32:56Z).
- `LLCircuitData::checkCircuitTimeout ... last ping 113.683s ... still dead, dropping`.
- `LLWatchdog ... mainloop expired` fired mid-stall.

## Cause 1: one slider change = ~6 full `setShaders()` reloads (upstream)

- `LLFeatureManager::setGraphicsLevel` writes many settings; each one with a
  `handleSetShaderChanged` listener (`RenderShadowDetail`, `RenderDeferredSSAO`,
  `RenderGlow`, ...) calls `LLViewerShaderMgr::setShaders()` synchronously, then
  `llfeaturemanager.cpp` calls it again.
- Log shows 6 "Loaded basic shaders" cycles for a single change.

## Cause 2: every reload is a full cache miss (upstream)

- `loadBasicShaders()` puts `SUN_SHADOW`, `SPOT_SHADOW`, `SSR`, `REFMAP_LEVEL`,
  `HERO_PROBES`, terrain defines into `LLGLSLShader::sGlobalDefines`.
- `LLGLSLShader::hash()` hashes `sGlobalDefines` into every program, so each
  intermediate settings state changes the hash of all ~330 programs.
- Shader cache works (binaries written to
  `%LOCALAPPDATA%\AyaneStormOS_x64\shader_cache`), it just never matches the
  transient intermediate states. A reload whose state was already seen took 0 s.

## Cause 3: AyaneStorm OIT shaders dominate compile time

Per-program timing from `.shaderbin` write times (one 20.3 s reload, 330 programs):

| Family | Programs | Seconds |
|---|---|---|
| AVBOIT | 25 | 8.3 |
| Mac OIT | 22 | 2.8 |
| Exact OIT | 19 | 2.2 |
| AyaneStorm post (Live DoF Gather alone 1.0 s) | 22 | 1.5 |
| All upstream/other | 242 | 5.7 |

- Heaviest single programs: `Material AVBOIT Shader 1/5/9/13` (~0.8-0.9 s each),
  `Skinned Deferred PBR Alpha AVBOIT` 0.76 s, `AyaneStorm Live DoF Gather` 1.0 s.
- `LLViewerShaderMgr::loadShadersDeferred` loads `ASAVBOIT`, `ASMacOIT` and
  `ASExactOIT` unconditionally, although `ASRenderOITMode` selects only one.
- 6 reloads x ~20-25 s = observed ~110 s. Without AS OIT: ~6 x 6 s.

## Fix options

1. Compile only the active OIT backend (gate on `ASRenderOITMode`; mode change
   must trigger `setShaders()`). Saves ~8-11 s per reload.
2. Coalesce reloads: defer `setShaders()` to one call per frame / after
   `setGraphicsLevel` finishes (upstream-touching, needs ownership tags).
3. Lazy-compile OIT material permutations on first use instead of at load.
4. Reduce AVBOIT material shader cost (largest per-program compile).

## Implementation (runtime tested 2026-10-27: ~4 s per change, no disconnect, down from ~107 s + disconnect)

Correction to cause 1: `setGraphicsLevel` already batches its own settings
(`LLViewerShaderMgr::sSkipReload`). The ~6 reloads come from the slider:
`LLSlider::handleHover` commits on every drag step, so each step ran
`setGraphicsLevel` and one full reload.

- **Settled slider** (`asgraphicslevelapply.*`): `LLFloaterPreference::onChangeQuality`
  and `FSFloaterPerformance::onChangeQuality` route through
  `ASGraphicsLevelApply::request()`; only the latest value runs, after mouse
  release and 0.3 s without change. One reload per slider move.
- **One reload per settled change** (runtime test 2026-10-27 showed the first build still reloaded 2-4 times):
  after `setGraphicsLevel()`'s reload (a cache hit), further settings written
  during the same apply (UI refresh / listeners) each called
  `handleSetShaderChanged` -> `setShaders()`, 1-3 times, every one a new
  intermediate state and a full cache miss (7-8 s blocking with no
  "Loaded cached binary" lines). A first fix batched only inside the slider
  apply; the next test still showed bursts of exactly six ~1 s reloads, one
  synchronous chain (listener -> buffer realloc -> `setShaders()`) outside
  both `sSkipReload` and that batch, source not identified. Replaced by a
  general merge (`asshaderreload.*`): `setShaders()` first calls
  `ASShaderReload::defer()`, which records the request while shaders are
  loaded (never at startup / GL restore, when callers need them at once);
  `display()` calls `ASShaderReload::flush()` right after
  `LLViewerWindow::checkSettings()` (upstream's own deferred reload point),
  before any frame work, so no frame renders between the request and the
  reload and the old programs stay valid meanwhile. The flush logs the
  request count and the define-relevant settings ("Shader reload for N
  request(s): shadows ... ssao ...") to diagnose cache misses.
- **Only the OIT families in use** (`asoitshaderloader.*`): `loadShadersDeferred`
  no longer compiles AVBOIT / Mac OIT / Exact OIT inline; it calls
  `ASOITShaderLoader::reset()`. Wanted families: Exact OIT if
  `ASExactOIT::isEnabled()`, AVBOIT if `ASAVBOIT::requested()`, Mac OIT if
  `ASMacOIT::shadersWanted()` (OIT mode 4, or Live DoF mode 3 for its bins).
  A family switched to later is compiled then; a family no longer wanted stays
  loaded once complete (instant switch back), and a pending one is cancelled.
- **Frame-budgeted compile**: each backend exposes `appendLoadSteps()` (one
  program or static/rigged pair per step) and `finishLoad()`.
  `ASOITShaderLoader::update()` runs from
  `ASOITDispatcher::refreshOrderIndependentAlphaState()` (before culling,
  outside rendering) and compiles steps for 20 ms per frame (at least one).
  While compiling, every backend's existing `shadersReady()` check keeps
  transparency on the vanilla path, and `orderIndependentAlphaActive()` stays
  false so vanilla alpha sorting runs.
- **Safety**: `LLViewerShaderMgr::unloadShaders()` calls
  `ASOITShaderLoader::invalidate()`, since `LLGLSLShader::unload()` clears the
  configured files/defines that queued steps would compile.
- Exact OIT material variants are now registered in
  `ASExactOIT::registerShaders()` instead of being pushed during load.
- An Exact OIT compile failure no longer fails the whole deferred load.

Expected cost per settled change on a cache miss: one vanilla reload (~6 s,
still blocking) plus the active OIT family spread over frames (UI and
networking keep running). Live DoF transparency bins and OIT are unavailable
(vanilla transparency) for those seconds, including in snapshots taken then.

Remaining blocking work: the vanilla `setShaders()` itself (242 programs) and
AS post shaders (Live DoF Gather ~1 s). Making those asynchronous needs
upstream `createShader()` changes (e.g. GL_KHR_parallel_shader_compile with
deferred status queries).

## Searchable terms

graphics slider freeze, quality preset stall, setShaders, shader cache miss,
sGlobalDefines hash, AVBOIT compile time, disconnect after graphics change,
checkCircuitTimeout, main loop block.
