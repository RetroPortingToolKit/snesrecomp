# SNES optimization burndown

Assessment: 2026-10-27. Owning roadmap: `beads-49cy`, under the snesrecomp
framework epic. Coordinating campaign: `beads-2wy8.1` (`optimization campaign`),
label `campaign:optimization-campaign`. Search and reuse owning issues before
starting work; claim the selected issue and record findings there and in the
campaign. Work solo unless the owner explicitly authorizes delegation.

## Landed baseline

- [Framework PR 219](https://github.com/RetroPortingToolKit/snesrecomp/pull/219)
  merged to main as `abdd665c5e9283775cc75e68fe4690ccd7c72aa5`.
- [MMX PR 148](https://github.com/mstan/MegaManXSNESRecomp/pull/148) merged to
  main as `e5d1308dfe51f7e3a47ec9fcef611f9d893f04e3`, pinning merged engine
  implementation `9f111225d1751d4d4913c887ed42b3f206838be4`.
- Fresh Windows x64 NATIVE MMX builds enable whole-view row planning. Explicit
  OFF retains the previous NATIVE compositor; the separate LLE renderer and
  other-platform defaults remain available.
- Five balanced A/B pairs per profile on an i5-4670 measured additional median
  process-CPU reductions of **7.4% custom 4:3**, **9.6% 16:9**, and **12.6% 32:9**.
  Ordinary stock rendering was **neutral/inconclusive**. These are a separate
  package from earlier campaigns; percentages must not be added together.
- Renderer suites, 512 randomized full-color rows, 4,096 shared visibility
  plans, eight captured view/HUD comparisons, all four Magic Carpet cases,
  current-build restore/rewind/replay and relevant CI pass. This does not
  establish original Xbox performance or cross-build netplay compatibility.

## Remaining opportunities

These are ranked opportunities, not promised gains or a requirement to implement
every hypothesis. Start with R1, then R2. Profile R3 before committing to a
replacement. R4 may join a coherent renderer package when attribution supports
it. Preserve individual passed, failed and unverified results.

| ID | Opportunity and scope | Next task and evidence required |
| --- | --- | --- |
| R1 | **Batch MMX background reconstruction.** Title adapter first; reusable pure span/decoding jobs may belong in the engine. | Prepare full-width scene-map, tile-art and palette bindings once per source span. After the landed row package, `background` remains the largest named hotspot: 533 of 2,543 main-thread samples in a verified 342px Highway workload, plus related helpers. This is hotspot evidence, not a speedup prediction. |
| R2 | **Adapt row planning to Super Metroid.** Reuse the shared planner with a Metroid-specific adapter. | Validate stock/wide rendering, HUD/windows/color math, room and door transitions, Mode 7 coexistence, interpolation, current-build restore and rewind. Measure the actual built game; the new row package has not been qualified there. |
| R3 | **Decode shared PPU layers once for main/subscreens.** Shared-engine consolidation hypothesis. | Profile a representative SMW/color-math workload first. `PpuDrawWholeLine` prepares backgrounds separately for the main and enabled subscreen. Determine how much work can actually be shared while preserving per-screen windows, priorities, overlays/enhancers, status and capture/readback. |
| R4 | **Avoid masked work and reuse unchanged raster setup.** MMX/shared row jobs where useful. | Current MMX rows skip globally disabled layers but prepare enabled rows before visibility spans. Assess preparing only the union needed by either screen and reusing setup across proven unchanged rows. Respect HDMA/IRQ/register/VRAM/scene boundaries and requested extra views. |
| R5 | **Batch remaining large-tile, offset-per-tile and mosaic paths.** Shared PPU, targeted games outside MMX. | Select a real hot workload first. Large-tile rendering remains per pixel; offset-per-tile selects authentic segments but still refetches tile/plane data inside each pixel. Test negative margins, flips, 16x16 midpoint changes, windows, mosaic and shadow/capture consumers. Ordinary 2/4bpp and opt-in 8bpp span implementations already exist. |
| R6 | **Audit and finish Doom's existing 8bpp adoption.** Reuse `beads-8wg.2.115` and merged PR 151. | Confirm the current delivered build's effective selection first. Windows gameplay/performance and owner qualification already exist; the shared selector remains LLE, and the inspected title CMake/tools/workflow sources had no explicit HLE selection. Complete integration only if missing; close as already done if delivery enables it. Do not count old gains again. |
| R7 | **Finish Super Mario Kart DSP-1 qualification.** Existing issue `beads-qhnd`; framework PR 150 is already merged. | Complete lap/finish/results/next-race checks, listening and clean-host performance measurement. Contracts and paired driving runs already exist. Previous noisy timings do not establish final acceptance or justify a broader default promotion. |
| R8 | **Select further whole-subsystem replacements from a demonstrated bottleneck.** Conditional, title-specific investigation. | Coprocessor jobs, idle waits, music-driver transactions and GPU rendering are legitimate boundaries. Choose one only when a useful workload demonstrates dominant cost; reuse existing issues/implementations. Define outputs, status/IRQ/readback, PCM and guest-continuation contracts. Assess software batching before a GPU rewrite; worker pools are not presumed useful on a single-core Xbox. |
| R9 | **Establish the original Xbox/32-bit budget.** Platform qualification, separate from desktop results. | Measure resident/peak memory, row-stack use, code/cache pressure and audio/render costs on the supported target. Account for assets, caches and rewind; reduce the largest demonstrated stores first. Actual Xbox qualification remains unverified. |

## Acceptance and stopping rules

1. Keep the shared framework correct and broadly usable; place game-specific
   scene, asset and camera policies in title adapters. Share pure mechanisms
   when they have real consumers rather than moving the whole MMX renderer.
2. Maintain a functioning build-selected LLE/reference implementation under the
   shared [HLE policy](https://github.com/mstan/recomp-ai-rules/blob/main/HLE.md).
   Replace internal algorithms and subsystem boundaries freely within the
   caller contract. Do not add runtime switching or shadow execution obligations.
3. For R1, cover resource-phase boundaries, displaced peer views, doors,
   reflections, clamps, ripple, mosaic, fades and HUD behavior. Use existing
   captured/randomized renderer suites plus all four ROM-backed Magic Carpet
   gates: native, widescreen, ultrawide and no-shot negative control. Magic
   Carpet alone does not validate renderer effects or every gameplay feature.
4. Preserve current-build save/load, rewind and rollback correctness, native
   saves and settings. Old save-state compatibility is never required. Keep
   netplay identity checks; do not infer compatibility across builds or hosts.
5. Use matched built-game workloads, features, settings and hosts. Retain raw
   balanced A/B samples, source/binary identities, package labels, relevant
   memory/code costs and machine-readable per-game evidence. Keep expensive
   diagnostics out of performance runs. Follow [PERFORMANCE.md](PERFORMANCE.md).
6. Acceptance is per game: neutral or positive is good. Do not adopt a
   reproducible regression because other games improve. Small mixed results
   within variation are neutral/inconclusive. Identical original/candidate
   failures are shared defects, tracked separately.
7. After two unsuccessful improvement experiments, reassess and pivot. Defer
   work whose measured cost cannot repay its complexity. GPU, coprocessor and
   driver replacements remain conditional until their workloads justify them.
8. For new changes requiring human regression coverage, bundle compatible
   candidates and pass automated gates first, then prepare normal-window,
   normal-audio owner checks. Preserve observations and wait for the owner's
   approval before merging that new batch. Do not repeat cleared checks.

## Already completed or deprioritized

Diagnostic culling, cached frame composition, exact raster snapshot reuse,
deferred reference pixels, APU event spans, native Mode 1/OAM/window jobs,
slicing CRC and MMX whole-view row composition are baseline work. The earlier
14-opportunity campaign is dispositioned in `beads-8wg.2.154`; reuse its evidence.

Two palette/color-cache experiments and the earlier generic composition-helper
and CPU bus-guard candidates were rejected. Reopen them only with new evidence.
Blind DMA, additional dispatch/AOT roots and deeper audio HLE are lower priority
on the inspected workloads. Historical component shares are not current
post-row ceilings and cannot establish priorities for every SNES title.

## Evidence and continuation

Central Beads metadata is the durable index:

- `beads-8wg.2.161`: `optimization_campaign_results` and `landing` contain the
  qualified row package's raw measurements, build identities and merged heads.
- `beads-49cy`: `remaining_snes_burndown` contains this ranked assessment,
  per-opportunity gates and stopping conditions.
- `beads-2wy8.1`: coordinating cross-engine campaign; keep it updated as each
  selected opportunity gains evidence or is rejected.

Retained local evidence is under
`G:/Temp/Codex/snesrecomp/full-view-20261027/`: `campaign-results.json`,
`evidence/landing.json`, `evidence/post-row-profile.json`, and
`evidence/remaining-burndown.json`. The post-row profile is coarse main-thread
IP sampling of a 4,200-frame actual Highway route, not exclusive CPU accounting
or throughput measurement. Build caches were cleaned; exact measured binaries,
source worktrees and the isolated owner profile remain. No new implementation
experiment was started when this document was saved.
