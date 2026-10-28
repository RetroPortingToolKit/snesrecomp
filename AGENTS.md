# Agent entry point

## Optimization scope (owner direction, 2026-10-28)

Prioritize game-agnostic mechanisms in the shared snesrecomp engine. Each game
may improve or remain neutral; a reproducible regression in one game cannot
be offset by gains elsewhere. Equal percentage gains are not required.

When custom renderers duplicate a useful mechanism, reuse an existing shared
facility or elevate the mechanism into the engine with real consumer validation.
Keep game-specific scene, asset and camera policy in title adapters. Moving
game-specific code or adding an unused abstraction is not engine consolidation.

Finish the already-authorized MMX and Super Metroid candidates, retaining their
new optimization options as explicit opt-ins. Briefly assess X2/X3 reuse; any
substantial new title adoption belongs after the shared-engine burndown or is
deferred. Further discretionary game-specific work also belongs at the end.
See [the burndown](docs/OPTIMIZATION_BURNDOWN.md) for scope and evidence.

## HLE/LLE policy

For HLE/LLE replacements and accuracy/performance tradeoffs, follow the shared
[HLE policy](https://github.com/mstan/recomp-ai-rules/blob/main/HLE.md)
(`F:\Projects\recomp-template\HLE.md` in the owner's workspace).

Maintain a functioning LLE reference for the replaced behavior. Select HLE or
LLE at build time; preserve the caller contract while allowing different
algorithms, private state, internal timing and documented minute differences.
Keep exact-core validation requirements scoped to exact-core changes.

Measure representative whole-title host cost and validate affected gameplay
before promoting HLE defaults. Follow `docs/PERFORMANCE.md` for measurement
and `docs/DSP1_IMPLEMENTATIONS.md` for DSP-1 build selection. Do not infer
mobile or original Xbox qualification from desktop results.
