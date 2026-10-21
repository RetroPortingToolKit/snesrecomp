# Agent entry point

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
