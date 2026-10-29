# Agent entry point

## Optimization scope (owner direction, 2026-10-28)

Prioritize game-agnostic mechanisms in the shared snesrecomp engine. Each game
may improve or remain neutral; a reproducible regression in one game cannot
be offset by gains elsewhere. Equal percentage gains are not required.

When custom renderers duplicate a useful mechanism, reuse an existing shared
facility or elevate the mechanism into the engine with real consumer validation.
Keep game-specific scene, asset and camera policy in title adapters. Moving
game-specific code or adding an unused abstraction is not engine consolidation.

The already-authorized MMX/Super Metroid candidates and brief X2/X3 reuse
assessment are complete. Keep their new source optimization choices opt-in.
Current work is the remaining concrete shared-engine R8 questions and existing
shared DSP-1 consumer validation in SMK. New title-specific adoption is outside
this engine queue.
Owner update 2026-10-29 authorizes up to three concurrent item agents, one item
per agent, with isolated worktrees/evidence and independent result review.
See the [full native/HLE burndown](docs/NATIVE_HLE_BURNDOWN.md) for the current
original-list reconciliation and remaining engine work. The nine-item
[follow-up register](docs/OPTIMIZATION_BURNDOWN.md) is historical context, not
a completion percentage for the full campaign. Owner direction 2026-10-29:
reject concrete proposals that show no value; explicitly defer further
Xbox/platform and dedicated memory-budget work. Games remain test consumers;
new title-specific adoption is outside the engine queue. Publish at the end.

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
