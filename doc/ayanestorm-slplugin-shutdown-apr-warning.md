# SLPlugin shutdown APR access-denied warning

Author: chanayane@firestorm

## Evidence: 2026-10-17 AyaneStorm.log

At 20:42:06 UTC, shutdown logs two consecutive termination attempts for
`SLPlugin.exe (26640)`: first an unnamed caller, then `destructor`.
The second attempt reports `llprocess.cpp:833 Accès refusé` through APR.
Cleanup continues to `Goodbye!` and `status: stopped` at 20:42:25 UTC.

## Code and likely cause

`indra/llcommon/llprocess.cpp`: `LLProcess::kill()` calls `apr_proc_kill()`
without immediately changing the cached running state. `getStatus()` returns
that cached state; `handle_status()` updates it when APR reports termination.
The destructor calls `kill("destructor")` for an attached process.
`indra/llplugin/llpluginprocessparent.cpp` also explicitly kills its child
during cleanup and destruction.

The sequence strongly indicates a duplicate termination attempt after the
first attempt already terminated the child. Microsoft documents that
`TerminateProcess()` returns `ERROR_ACCESS_DENIED` when called on an already
terminated process:
https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess

## Assessment

Likely harmless shutdown noise in this occurrence. The log does not prove the
child's final process state, but shows viewer cleanup completed. The source
path in the warning identifies the call site, not a denied file access.
No source changes or build required for this diagnosis.

## Slow logout investigation: same day's two sessions

The warning is harmless, but successful cleanup does not imply fast cleanup.
The server replied to logout within the same timestamp second in both logs.

| Phase | AyaneStorm.log (20:42 UTC) |
| --- | --- |
| Logout request to stopped | 19 seconds (20:42:06–20:42:25) |
| Inventory saving and early disconnect work | About 2 seconds (06–08) |
| Avatar/scene teardown within disconnectViewer | About 7 seconds (08–15) |
| Remaining cleanup | About 10 seconds (15–25) |
| Vertex-buffer cleanup, within remaining cleanup | About 3 seconds (20–23) |

`AyaneStorm.old` shows another slow exit on 2026-10-17: 19:58:02–19:58:30
(28 seconds), with `disconnectViewer()` occupying about 18 seconds.
This establishes repeated slow local cleanup, rather than a server logout
response delay. The duplicate plugin termination has no visible timestamp
gap in the current log.

The avatar messages come from `LLVOAvatar::~LLVOAvatar()`, so they identify
ongoing destruction, not avatars loading during logout. They do not establish
that logging itself causes the delay. `LLWorld::resetClass()` and object
destruction are the primary area to profile. Vertex-buffer cleanup deletes
`sVBOPool`; its exact internal cost is not resolved by existing log entries.

An older retained log dated 2025-09-22 reaches OpenGL shutdown within the
logout timestamp second, but lacks `Goodbye!` and `status: stopped`; it is
not sufficient to establish a complete historical exit-time baseline.
Existing logs do not identify the responsible code change. No source edits
or build performed during this investigation.
