# Review: trish-sl texture fetch/cache patches

Third-party Firestorm fork commits reviewed for possible adoption (2026-11-02).

## Status

"Parts worth taking" ported 2026-11-02, tagged `// <AS:Chanayane> authored by Trish (trish-sl)`. Everything under "Parts to skip or defer" and c58fb10 left out. Adaptations:
- `LLTextureFetchWorker::getRetryDelayMs()` without the transient HTTP retry branch (`mHttpRetryAttempt`, skipped feature).
- In-memory entry info (`mEntryInfos`) kept with upstream timestamp policy (`MAX_ENTRIES_WITHOUT_TIME_STAMP` early return kept, no periodic header writes, no purge rework); `updateEntryTimeStamp` also updates `mEntryInfos` so it never goes stale.
- `readHeaderCache` partial sort covers at least one LRU entry (`llmax(lru_entries, 1)`), matching the original loop.
- Region re-resolve before send, cached-bytes decode shortcut and decode discard clamp in `doWork` not ported.

Files: `llqueuedthread.*`, `llworkerthread.*`, `tests/workqueue_test.cpp`, `_httpinternal.h`, `lltexturefetch.cpp`, `lltexturecache.*`, `llviewertexturelist.cpp`.

- `f070ca1df344bdf33ebc81a1ecc905aa0ea9ac6e` "Bundle of rendering pipeline, fetch and cache improvements" (16 files, +1343/-213)
- `c58fb10612e71e05ab05e6418ea6db7fbdd3a277` "EXPERIMENTAL - Include overrides in tex refresh" (llviewermenu.cpp, +26)

Base matches our tree (FS:Beq deferred-retry code in `llqueuedthread.cpp`, `HTTP_RETRY_BACKOFF_MAX_DEFAULT = 50000006UL` in `_httpinternal.h`).

## Scope

Texture streaming latency (teleport arrival, fast travel, region crossing) and worker-thread CPU waste. Not frame rate, not rendering, not GPU offload. Main thread cost is roughly unchanged (some budget moved between passes).

## Parts worth taking

- **HTTP retry backoff typo** (`_httpinternal.h`): `50000006UL` -> `5000000UL`. Real bug (max backoff 50 s clamped to 20 s instead of 5 s). One line.
- **Timed retry scheduling** (`llqueuedthread.*`, `llworkerthread.*`): `mRequestQueue` becomes `LL::WorkSchedule`, deferred requests posted with their due time instead of re-posting every 16 ms and `ms_sleep(2)`. Parked texture workers (thousands after a teleport) stop busy-polling the fetch/cache threads. `ThreadSafeSchedule::popWithTime` wakes on push, so ready work is not held behind deferred work (patch adds a unit test). Caveats: FS:Beq's old comment said future-time posts caused 10+ ms waits (likely Windows timer granularity, affects only deferred items); shutdown `runUntilClose` now waits for deferred items (up to ~250 ms). Per-worker `getRetryDelayMs()` (50 ms for HTTP slot wait, up to 250 ms for capability/retry waits).
- **`releaseHttpWaiters` refill**: refill when 1/8 of slots free instead of below low water. Keeps the HTTP pipe full.
- **HTTP slot leak fixes**: release semaphore on capability wait, 404/503 -> INIT paths, low-priority abort from WAIT_HTTP_RESOURCE*/SEND_HTTP_REQ with state rewind.
- **Texture cache read path**: body size from entry instead of `LLFile::size()` per read; in-memory entry time/size (`mEntryInfos`) to avoid header file reads; `unordered_map` for id/size maps; `partial_sort` at startup; fast cache file kept open.
- **Animated texture resolution** (`updateImageDecodePriority`): use frame size of texture anim for min_scale. Small, isolated.

## Parts to skip or defer

- **`updateRequestPriority` made synchronous**: main thread now takes `mQueueMutex` + worker `mWorkMutex` per texture. `LLTextureFetchWorker::doWork` holds `mWorkMutex` for its whole run (lltexturefetch.cpp `doWork`), so the main thread can block on the fetch thread. Upstream posts it async for this reason.
- **Downscale only when `sDesiredDiscardBias > 1`**: keeps full-res textures in VRAM until pressure. Less hitching, more VRAM. Policy change, needs testing on low-VRAM GPUs.
- **`RenderTextureMemoryUsageBias`** (default 1.8): replaces the hard-coded x2 (`/512`) estimate with x1.8 (`/1024 * 1.8`), i.e. ~10% more VRAM allowed. Name lacks `AS` prefix.
- **Cache budget rework**: headers and bodies share one budget, header-only entries now purged, 10% purge instead of 20%. Changes cache eviction behavior for existing users.
- **Transient-failure path in `processFetchResults`**: texture-level re-request on 5xx/429/abort has no cap; a persistently failing asset can be re-requested indefinitely (worker caps at 3 retries per request only).
- **Two extra round-robin scans in `updateImagesFetchTextures`** (urgent lane, fetch priority lane, up to 2048 `std::map` entries each, `upper_bound` per step): time-bounded but adds main-thread work; value unmeasured.
- **Teleport arrival decode budget 15 ms**: fine while the arrival screen is up; harmless.
- **c58fb10**: Texture Refresh also calls `addCacheMissFull()` on objects with render material params so the sim resends GLTF overrides. Low risk, low value, marked experimental.

## Merge cost

All edits are in `ll*` files, many untagged, total ~1300 lines across `lltexturefetch`, `lltexturecache`, `llviewertexture*`. LL actively changes these files; conflicts on every upstream merge. Our rules require `AS:Chanayane` tags with original code kept commented. Adopt piecemeal, not wholesale.

## Measurement before adopting

- Built-in `TextureFetchPipelineDiagnostics` (from the patch) logs pipeline state every 5 s.
- Compare time-to-sharp after teleport to a dense region, and fetch/cache thread CPU in Tracy, before/after the timed-retry part.
