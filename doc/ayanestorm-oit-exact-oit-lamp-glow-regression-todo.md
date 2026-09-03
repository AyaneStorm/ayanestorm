# Exact OIT: lamp glow regression (TODO, not yet investigated)

Author: chanayane@firestorm. Date: 2026-09-03.

## Symptom

A wall-mounted lamp's glass renders lit (glowing) in vanilla and in AVBOIT,
but unlit (dark) in Exact OIT. Screenshots compared side by side confirm
the difference: Exact OIT's lamp glass has no glow contribution, vanilla
and AVBOIT both show the lit glow.

## Suspected cause (not verified)

Not yet investigated. The most recent Exact OIT change this session that
touches glow semantics is E2-B (`doc/ayanestorm-oit-performance-audit-plan.md`,
committed): `mHasGlow` was changed from "wrote any glow data" (always true
for particles) to "wrote actually non-zero glow data", and
`FSExactOIT::handleCapturedEmissives()` now drops draws with `mHasGlow ==
false` before dispatching to the capture emissive shaders.

If the lamp's light source is implemented as a static prim with an
emissive texture/glow (not a particle), it's unclear yet whether E2-B's
change could affect it — E2-B's `mLastGlowNonZero` flag is specific to
`LLVOPartGroup`, and non-particle objects were expected to keep `mHasGlow`
already true (per E2's own trap: "Verify prims (LLVOVolume) are unaffected:
they only have TYPE_EMISSIVE when glow > 0, so mHasGlow stays true for
them"). If the lamp is instead a static prim, this theory does not apply
and the actual cause is unknown.

## Status

Not investigated. User explicitly asked to defer this ("TODO for later").
No code changes made. Revisit when the user is ready to pick it up.

## Repro

Wall-mounted lamp, glass should glow/be lit. Compare Standard (vanilla),
AVBOIT, and Exact OIT rendering modes side by side.
