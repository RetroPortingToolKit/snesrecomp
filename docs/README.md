# snesrecomp documentation

Every document in this folder, grouped by what you would be doing when you need
it. Ten of these were reachable from nothing before this index existed,
including the two longest, so the point of this file is that no document is
findable only by knowing it is there.

**Status markers** are the document's own, copied here so a title alone does not
have to tell you whether something is shipped or proposed. A document with no
marker describes the tree as it is.

## Building a game on the framework

| Document | |
|---|---|
| [`GAME_PROJECT_SETUP.md`](GAME_PROJECT_SETUP.md) | Start here. Laying out a game repo, what it owns vs. what the framework owns, and the rules that keep a rename from breaking your build. |
| [`LOCAL_CODEGEN_SDK.md`](LOCAL_CODEGEN_SDK.md) | The player-side Generate & Play path: exit codes, packaging, and what ships. |
| [`RUNNER_LAYOUT.md`](RUNNER_LAYOUT.md) | What each `runner/src` layer folder holds and where a new file goes. Read before moving anything under `runner/src`. |
| [`LAUNCHER_DESIGN.md`](LAUNCHER_DESIGN.md) | What the launcher is, what it owns, and what was removed from it. |
| [`LAUNCHER_CORE.md`](LAUNCHER_CORE.md) | *Design.* snesrecomp as a core hosted by a launcher, and the boundary that would make that possible. |
| [`SDL_BACKENDS.md`](SDL_BACKENDS.md) | Choosing SDL3 or the SDL2 fallback, and the game benchmark mode. |

## Game-facing features

| Document | |
|---|---|
| [`MOD_PACKAGES.md`](MOD_PACKAGES.md) | Mod package format, trusted static plugins, and the trust model. |
| [`MSU1.md`](MSU1.md) | MSU-1 registers, pack resolution, and what a game must integrate. |
| [`MULTITAP.md`](MULTITAP.md) | Super Multitap, up to 8 players. |
| [`RUNTIME_LOCALIZATION.md`](RUNTIME_LOCALIZATION.md) | Translating game text at runtime. |
| [`WIDESCREEN_PATTERNS.md`](WIDESCREEN_PATTERNS.md) | The invariants a widescreen port must reproduce, and why the shared hook layer is still proposed rather than proven. |
| [`SHADOW_ENHANCEMENTS.md`](SHADOW_ENHANCEMENTS.md) | Verified-shadow audio and screen enhancements: what is done, what is engine-agnostic. |
| [`HOST_OVERLAY_EXTRACTION.md`](HOST_OVERLAY_EXTRACTION.md) | Pulling host overlays out of the guest frame. |
| [`IMPROVEMENTS.md`](IMPROVEMENTS.md) | Framework-level improvements, accepted and rolled back, with attribution. |

## Correctness and accuracy

| Document | |
|---|---|
| [`ISSUES.md`](ISSUES.md) | Known runner- and recompiler-level issues. |
| [`SNES_ACCURACY_BURNDOWN.md`](SNES_ACCURACY_BURNDOWN.md) | The accuracy axes, what is measured, and what is still open. Deliberately names some files that exist on no checkout. |
| [`SNES_COSIM.md`](SNES_COSIM.md) | Full-state first-divergence co-simulation: design and validation gates. |
| [`TRIPWIRES.md`](TRIPWIRES.md) | Runtime tripwires and how to query one. |
| [`ABSTRACT_INTERPRETATION_GAPS.md`](ABSTRACT_INTERPRETATION_GAPS.md) | *Living document.* Decoder soundness reference. |
| [`ANALYZER_GAPS_INVENTORY.md`](ANALYZER_GAPS_INVENTORY.md) | *Living document.* Every known 65816 static-recompilation gap, with status. |
| [`LLE_FIRST_ANALYSIS.md`](LLE_FIRST_ANALYSIS.md) | Whole-program analysis under an LLE-first model. |
| [`FRAME_MODEL_TIMING.md`](FRAME_MODEL_TIMING.md) | *Scoped, not implemented.* Frame-model interrupt timing. |
| [`FRAME_MODEL_HOSTS.md`](FRAME_MODEL_HOSTS.md) | Which hosts drive the frame model. |

## Performance

| Document | |
|---|---|
| [`PERFORMANCE.md`](PERFORMANCE.md) | The burn-down: current baseline, measurement contract, retained results, and rejected experiments. |
| [`BENCHMARKING.md`](BENCHMARKING.md) | Benchmark output format and the full final-validation sequence. The longest document here; use its section headings. |
| [`MULTI_TIER.md`](MULTI_TIER.md) | *Design accepted, implementation not started.* AOT → DLL shard → JIT → interpreter, and the manifest that folds runtime discoveries back into the build. |
| [`LLE_SCHEDULER.md`](LLE_SCHEDULER.md) | Path to "rich CFG × LLE": compiled tasks and the scheduler. |

## Netplay

| Document | |
|---|---|
| [`RECOMP_NET.md`](RECOMP_NET.md) | The netcode layer, its transport, and where fixes belong. |
| [`ROLLBACK.md`](ROLLBACK.md) | SNES rollback netcode. |

## Tooling, spikes and records

| Document | |
|---|---|
| [`LUA_TCP.md`](LUA_TCP.md) | *Spike.* Driving the runner over a Lua TCP bridge. |
| [`ASSET_TOOLS_PLAN.md`](ASSET_TOOLS_PLAN.md) | *Plan.* Asset dumping and function identification. |
| [`ARWING64_INTEGRATION.md`](ARWING64_INTEGRATION.md) | Record of the Arwing64 engine integration. |
| [`DKC2_BRANCH_MERGE_ACCEPTANCE.md`](DKC2_BRANCH_MERGE_ACCEPTANCE.md) | Acceptance record for the DKC2 branch merge. |
| [`ci/README.md`](ci/README.md) | What the CI workflows do. |

## Elsewhere in the repo

- [`../README.md`](../README.md) — what snesrecomp is, and the quick start.
- [`../DEVELOPMENT.md`](../DEVELOPMENT.md) — working on the framework itself.
- [`../THIRD_PARTY_ATTRIBUTION.md`](../THIRD_PARTY_ATTRIBUTION.md) — every vendored or adapted source and its licence.
- [`../tools/README.md`](../tools/README.md) — **which `tools/` scripts other repositories invoke by path.** Read before moving one.
