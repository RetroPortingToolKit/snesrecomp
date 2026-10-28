# SNES optimization burndown

Assessment: 2026-10-27; scope revised by the owner on 2026-10-28.
Owning roadmap: `beads-49cy`, under the snesrecomp
framework epic. Coordinating campaign: `beads-2wy8.1` (`optimization campaign`),
label `campaign:optimization-campaign`. Search and reuse owning issues before
starting work; claim the selected issue and record findings there and in the
campaign. Work solo unless the owner explicitly authorizes delegation.

## Shared-engine priority

Future optimization work belongs in game-agnostic shared engine mechanisms.
Every measured game must be neutral or better; equal gains are not required.
If a custom renderer duplicates a useful operation, reuse or elevate the shared
mechanism and validate actual consumers. Preserve title scene/asset/camera
policy in adapters. Do not describe a title adapter's measured gain as an
engine-wide gain, and do not add abstractions without demonstrated consumers.

The owner explicitly authorized finishing the existing MMX and Super Metroid
candidates. Their new optimization switches remain OFF by default; automated
qualification does not promote defaults. Briefly assess whether X2/X3 can
reuse the work directly. Substantial new title adapters, pin adoption and
other discretionary title-only work go after the shared-engine burndown or
are deferred. Existing renderer selections and the functioning LLE floor stay
available.

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

These are opportunities, not promised gains or a requirement to implement every
hypothesis. Keep the IDs stable. Close out the already-authorized R1/R2 and R4
Metroid candidate, then prioritize shared work R5, R8 and R9. R3's measured SMW
hypothesis is deferred. Move R6/R7 title-only adoption checks and substantial
X2/X3 work to the final adoption queue. Preserve individual passed, failed and
unverified results.

| ID | Opportunity and scope | Next task and evidence required |
| --- | --- | --- |
| R1 | **Batch MMX background reconstruction.** Title adapter first; reusable pure span/decoding jobs may belong in the engine. | Prepare full-width scene-map, tile-art and palette bindings once per source span. After the landed row package, `background` remains the largest named hotspot: 533 of 2,543 main-thread samples in a verified 342px Highway workload, plus related helpers. This is hotspot evidence, not a speedup prediction. |
| R2 | **Adapt row planning to Super Metroid.** Reuse the shared planner with a Metroid-specific adapter. | Validate stock/wide rendering, HUD/windows/color math, room and door transitions, Mode 7 coexistence, interpolation, current-build restore and rewind. Measure the actual built game; the new row package has not been qualified there. |
| R3 | **Decode shared PPU layers once for main/subscreens.** Shared-engine consolidation hypothesis. | Profile a representative SMW/color-math workload first. `PpuDrawWholeLine` prepares backgrounds separately for the main and enabled subscreen. Determine how much work can actually be shared while preserving per-screen windows, priorities, overlays/enhancers, status and capture/readback. |
| R4 | **Avoid masked work and reuse unchanged raster setup.** MMX/shared row jobs where useful. | Current MMX rows skip globally disabled layers but prepare enabled rows before visibility spans. Assess preparing only the union needed by either screen and reusing setup across proven unchanged rows. Respect HDMA/IRQ/register/VRAM/scene boundaries and requested extra views. |
| R5 | **Batch remaining large-tile, offset-per-tile and mosaic paths.** Shared PPU, targeted games outside MMX. | Select a real hot workload first. Large-tile rendering remains per pixel; offset-per-tile selects authentic segments but still refetches tile/plane data inside each pixel. Test negative margins, flips, 16x16 midpoint changes, windows, mosaic and shadow/capture consumers. Ordinary 2/4bpp and opt-in 8bpp span implementations already exist. |
| R6 | **Audit and finish Doom's existing 8bpp adoption.** Reuse `beads-8wg.2.115` and merged PR 151. | Confirm the current delivered build's effective selection first. Windows gameplay/performance and owner qualification already exist; the shared selector remains LLE, and the inspected title CMake/tools/workflow sources had no explicit HLE selection. Complete integration only if missing; close as already done if delivery enables it. Do not count old gains again. |
| R7 | **Finish Super Mario Kart DSP-1 qualification.** Existing issue `beads-qhnd`; framework PR 150 is already merged. | Complete lap/finish/results/next-race checks, listening and clean-host performance measurement. Contracts and paired driving runs already exist. Previous noisy timings do not establish final acceptance or justify a broader default promotion. |
| R8 | **Select further shared whole-subsystem replacements from a demonstrated bottleneck.** Game-agnostic engine work, validated in actual titles. | Coprocessor jobs, idle waits, music-driver transactions and GPU rendering are legitimate boundaries. Choose one only when a useful workload demonstrates dominant cost; reuse existing issues/implementations. Elevate reusable operations from custom renderers where justified, retaining title policy in adapters. Define outputs, status/IRQ/readback, PCM and guest-continuation contracts. Assess software batching before a GPU rewrite; worker pools are not presumed useful on a single-core Xbox. |
| R9 | **Establish the original Xbox/32-bit budget.** Platform qualification, separate from desktop results. | Measure resident/peak memory, row-stack use, code/cache pressure and audio/render costs on the supported target. Account for assets, caches and rewind; reduce the largest demonstrated stores first. Actual Xbox qualification remains unverified. |

## Current dispositions (2026-10-28)

| ID | Status and evidence |
| --- | --- |
| R1 | MMX title adapter automated-qualified, runtime `ef821a32`, documentation head `90916f7`, draft MMX PR 149; owner combined playtest and merge remain. New `MMX_NATIVE_BG_SPANS` default OFF. Five paired i5-4670 samples/profile: median CPU reduction 12.81% at 16:9, 34.69% at 32:9; custom 4:3 and stock neutral/inconclusive. All four Magic Carpet gates pass. This is a title result, not a new shared-engine implementation. |
| R2 | Super Metroid adoption of the existing shared row planner automated-qualified, runtime `aa66ec96`, documentation head `bf73de9`, draft Metroid PR 38; owner combined playtest, interactive rewind and merge remain. New `SM_NATIVE_ROW_PLAN` default OFF. Five paired i5-4670 samples/profile: median CPU reduction 8.86% custom 4:3, 5.56% at 16:9; stock/32:9 neutral/inconclusive. |
| R3 | Investigation complete; implementation deferred for measured SMW workload. Both stock/wide routes had zero same-line main/sub background-layer overlap. Reopen for a demonstrated hot overlapping consumer, not merely two draw calls. Owning issue `beads-8wg.2.164`. |
| R4 | Existing Metroid native-center reuse adapter automated-qualified, measured runtime `914d518`; combined owner playtest/interactive rewind and merge remain. New `SM_NATIVE_CENTER_REUSE` default OFF. Five paired i5-4670 samples/profile against R2: median CPU reduction 16.28% custom 4:3, 12.36% at 16:9, 7.95% at 32:9, all pairs positive. These require trusted matching NewRenderer/NoSpriteLimits=1 input at discrete alpha; interpolation and unsupported cases keep the full custom route. NoSpriteLimits=0 fallback and stock are neutral/inconclusive. Do not add these percentages to R2's different NoSpriteLimits=0 package. Windows/Linux fixtures, live effects/extra presentations, production same/fresh-process restore and matched run-ahead pass. Shared owner `beads-8wg.2.165`, title owner `beads-8wg.6.36`. This closes the grandfathered title experiment, not all future shared R4 possibilities. |
| R5 | Source screen complete; implementation deferred under `beads-8wg.2.166`. Nineteen retained real Metroid captures contain 4,256 nonblank rows and 11,868 enabled background-layer rows, with zero enabled large-tile/OPT/mosaic rows. This is selected-scene evidence, not a time-weighted profile or an all-game conclusion. Reopen for a measured hot built-game workload; the known Metal Warriors source consumer has no local ROM/build and does not justify a new title port here. |
| R8 | Shared subsystem replacement remains conditional on a demonstrated dominant cost. Reuse post-optimization built evidence before selecting another boundary. |
| R9 | Shared compact captured-register interface implemented and fixture-qualified under `beads-8wg.2.167`; matched SM/MMX/SMW built-game qualification remains. [Interface and compiler costs](RASTER_REGISTERS.md): Metroid EndFrame stack 412,352 -> 1,456 bytes, Draw 214,432 -> 13,040, renderer BSS -205,440 bytes. Follow-up `beads-8wg.2.168` gives the [scene export's scratch copy](SCENE_EXPORT_MEMORY.md) a bounded transient lifetime, removing a 1,572,864-byte static array on the measured ABI while retaining complete pre-query histories. Windows/Linux snapshot/failure contracts pass; combined built-game qualification remains. No runtime CPU/FPS or Xbox claim yet. Actual target budget and other diagnostic-store contracts remain separate leads. |
| R6, R7 | Final title-adoption/qualification queue; reuse existing work and do not count historical gains again. |

### Final adoption queue

After shared-engine opportunities are dispositioned, audit Doom's effective
8bpp selection (R6), finish the existing Mario Kart DSP-1 title qualification
(R7), and consider substantial X2/X3 adoption only if the brief assessment
shows it needs new work. No speculative new title renderer rewrites belong
ahead of shared work. Record deferred items with a concrete reopening trigger.

The brief X2/X3 assessment is complete: current X2 master `9c3b374c` and X3
master `e88e5aab` both use the shared live PPU plus a shared Mmx23 margin
provider, rather than MMX's custom renderer. The X1 span build flag is not a
drop-in adoption. It embeds X1 scene/WRAM/private-CHR/palette rules. Defer a
substantial port until a measured hot shared provider justifies a reusable
span interface; optimize that mechanism in the engine and keep title policies
in adapters. Their existing correctness work and differing engine pins are
untouched. No new X2/X3 gain is claimed. Source hashes/extracts and disposition
are in `native-center-reuse-20261028/evidence/x2-x3-adoption-assessment.json`.

## Acceptance and stopping rules

1. Keep the shared framework correct and broadly usable. Future optimization
   mechanisms must be game-agnostic and shared; place game-specific scene,
   asset and camera policies in title adapters. Share pure mechanisms with
   validated consumers rather than moving the whole MMX renderer. Finish only
   the explicitly grandfathered title candidates before the shared queue.
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
source worktrees and the isolated owner profile remain. That directory records
the landed baseline, not the subsequent candidates.

Subsequent candidate evidence: `background-spans-20261027/campaign-results.json`
(R1), `sm-row-plan-20261028/campaign-results.json` (R2),
`shared-screen-decode-20261028/campaign-results.json` (R3), and
`native-center-reuse-20261028/campaign-results.json` (R4), beneath the same
task parent. Package percentages are not additive. No Xbox or cross-build
netplay claim is implied. Raw paired measurements and exact source/binary/
runtime identities are retained for the eventual campaign visualizations.

Shared R5 screening and R9 compact register work are retained under
`shared-render-registers-20261028/evidence/`, including
`r5-source-screen.json`, `stack-compare.json`, `campaign-milestone.json` and
the matched baseline/candidate source and binary records as qualification
progresses. Compiler cost results do not substitute for built-game acceptance.
