# SNES optimization follow-ups

The full current [native/HLE burndown](NATIVE_HLE_BURNDOWN.md) contains the
original 14-item checklist, the broader engine assessment and remaining engine
questions. This nine-item follow-up register is not the whole campaign.

Assessment: 2026-10-27; dispositions updated on 2026-10-29.
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
other discretionary title-only work are outside the current shared-engine
queue. Xbox/platform and dedicated memory-budget work are deferred by the owner. Existing renderer selections and the functioning LLE floor stay
available.

Final checkpoint: shared PRs 224–230 are merged as `35f923a7`, with a tree
identical to qualified `be97ec06`. The owner cleared the combined MMX,
Metroid and SMW builds. R8's specific investigations are complete; retained
and rejected proposals, per-game measurement limits and remaining SMK
consumer checks are recorded in the [current checkpoint](NATIVE_HLE_BURNDOWN.md#final-shared-package-checkpoint-2026-10-29).
The owner additionally requested cumulative before/after measurements from
the pre-October-22 game/engine pairings (`beads-8wg.2.185`). Publication remains
at the endpoint. Earlier pending statuses below are dated experiment history.

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

These are the historical nine follow-up candidates, not a count of the full
native/HLE campaign. R8 is reopened to evaluate remaining concrete shared-engine
costs; it is not restricted to finding a dominant whole-subsystem rewrite.
R7 is separate validation of the existing shared DSP-1 implementation using SMK.
R6 title adoption is outside this engine queue. The owner explicitly defers
Xbox/platform and dedicated memory work. Concrete R3/R5 proposals that showed
no value in the inspected workloads are rejected, without an all-game claim.
The combined Metroid owner check remains cleared. Keep individual passed,
failed and unverified results; a completed screen is not a shipped optimization.

| ID | Opportunity and scope | Next task and evidence required |
| --- | --- | --- |
| R1 | **Batch MMX background reconstruction.** Title adapter first; reusable pure span/decoding jobs may belong in the engine. | Prepare full-width scene-map, tile-art and palette bindings once per source span. After the landed row package, `background` remains the largest named hotspot: 533 of 2,543 main-thread samples in a verified 342px Highway workload, plus related helpers. This is hotspot evidence, not a speedup prediction. |
| R2 | **Adapt row planning to Super Metroid.** Reuse the shared planner with a Metroid-specific adapter. | Validate stock/wide rendering, HUD/windows/color math, room and door transitions, Mode 7 coexistence, interpolation, current-build restore and rewind. Measure the actual built game; the new row package has not been qualified there. |
| R3 | **Decode shared PPU layers once for main/subscreens.** Shared-engine consolidation hypothesis. | Profile a representative SMW/color-math workload first. `PpuDrawWholeLine` prepares backgrounds separately for the main and enabled subscreen. Determine how much work can actually be shared while preserving per-screen windows, priorities, overlays/enhancers, status and capture/readback. |
| R4 | **Avoid masked work and reuse unchanged raster setup.** MMX/shared row jobs where useful. | Current MMX rows skip globally disabled layers but prepare enabled rows before visibility spans. Assess preparing only the union needed by either screen and reusing setup across proven unchanged rows. Respect HDMA/IRQ/register/VRAM/scene boundaries and requested extra views. |
| R5 | **Batch remaining large-tile, offset-per-tile and mosaic paths.** Shared PPU, targeted games outside MMX. | Select a real hot workload first. Large-tile rendering remains per pixel; offset-per-tile selects authentic segments but still refetches tile/plane data inside each pixel. Test negative margins, flips, 16x16 midpoint changes, windows, mosaic and shadow/capture consumers. Ordinary 2/4bpp and opt-in 8bpp span implementations already exist. |
| R6 | **Audit and finish Doom's existing 8bpp adoption.** Reuse `beads-8wg.2.115` and merged PR 151. | Confirm the current delivered build's effective selection first. Windows gameplay/performance and owner qualification already exist; the shared selector remains LLE, and the inspected title CMake/tools/workflow sources had no explicit HLE selection. Complete integration only if missing; close as already done if delivery enables it. Do not count old gains again. |
| R7 | **Finish Super Mario Kart DSP-1 qualification.** Existing issue `beads-qhnd`; framework PR 150 is already merged. | Complete ordinary HLE/LLE lap/finish/results/next-race and listening checks. Matched capacity-pool performance and prior current-build state contracts already exist; no idle-host requirement or broader default promotion. |
| R8 | **Evaluate remaining measurable shared native/HLE opportunities.** Reopened engine investigation. | Work serially through shared rendering, execution/bus/DMA, SPC/DSP audio, and scheduling/coprocessor questions in [the full burndown](NATIVE_HLE_BURNDOWN.md#remaining-engine-investigation-r8). Reuse prior results; no dominant-cost or large-rewrite requirement. Accept measured useful shared gains or reject a concrete proposal with its tested reason. |
| R9 | **Establish the original Xbox/32-bit budget.** Platform qualification, separate from desktop results. | Measure resident/peak memory, row-stack use, code/cache pressure and audio/render costs on the supported target. Account for assets, caches and rewind; reduce the largest demonstrated stores first. Actual Xbox qualification remains unverified. |

## Current results (2026-10-29)

| ID | Status and evidence |
| --- | --- |
| R1 | Complete: owner cleared, merged and released as X1 v2.3.9, source/tag `77d0cb3a`, current-main merge `9fd75989`. MMX PRs 149/152/153/154 are merged. One verified current ZIP is delivered. New `MMX_NATIVE_BG_SPANS` source default OFF; Windows release explicitly enables it. Five paired i5-4670 samples/profile: median CPU reduction 12.81% at 16:9, 34.69% at 32:9; custom 4:3 and stock neutral/inconclusive. All four Magic Carpet gates pass. This is a title result, not a new shared-engine implementation. |
| R2 | Complete: existing shared row planner adopted, owner combined gameplay/rewind cleared, integrated by Metroid PR 45 and shipped in stable v0.3.19 (PR 46). PR 38 was merged earlier; superseded integration PRs are closed. New `SM_NATIVE_ROW_PLAN` source default remains OFF. Original isolated runtime `aa66ec96`: five paired i5-4670 samples/profile, median CPU reduction 8.86% custom 4:3, 5.56% at 16:9; stock/32:9 neutral/inconclusive. These remain measurements of that package, not the later CI release executable. |
| R3 | **Rejected for the tested SMW workloads.** Both stock/wide routes had zero same-line main/sub background-layer overlap, and the custom renderer already decodes once. No useful work for the proposed consolidation was demonstrated. This is not an all-game theorem; do not repeat the same investigation without new evidence. Owning issue `beads-8wg.2.164`. |
| R4 | Complete: Metroid native-center reuse adapter qualified, owner combined gameplay/rewind cleared, integrated by PR 45 and shipped in stable v0.3.19. New `SM_NATIVE_CENTER_REUSE` source default remains OFF. Original measured runtime `914d518`: five paired i5-4670 samples/profile against R2, median CPU reduction 16.28% custom 4:3, 12.36% at 16:9, 7.95% at 32:9, all pairs positive. These require trusted matching NewRenderer/NoSpriteLimits=1 input at discrete alpha; interpolation and unsupported cases keep the full custom route. NoSpriteLimits=0 fallback and stock are neutral/inconclusive. Do not add these percentages to R2's different NoSpriteLimits=0 package. Windows/Linux fixtures, live effects/extra presentations, production same/fresh-process restore and matched run-ahead pass. Shared owner `beads-8wg.2.165`, title owner `beads-8wg.6.36`. This closes the grandfathered title experiment, not all future shared R4 possibilities. |
| R5 | **Rejected for the inspected Metroid workloads** under `beads-8wg.2.166`. Nineteen real captures contain 4,256 nonblank rows and 11,868 enabled BG-layer rows, with zero enabled large-tile/OPT/mosaic rows. These captures do not establish a universal ceiling or justify a new title port. Other shared rendering questions remain explicit in R8.1. |
| R8 | **Open, with corrected scope:** issue `beads-8wg.2.169` now covers remaining concrete shared native/HLE costs, not only dominant subsystem replacements. Earlier X1 stock/wide samples did not exhaust that question. The full burndown lists four serial cost questions and requires implementation/measurement or evidence-backed rejection of each concrete proposal. Old samples remain independent selection observations, not matched throughput or proof that no gains remain. |
| R9 | Shared compact registers (`beads-8wg.2.167`, framework PR 221) and bounded scene-export scratch (`beads-8wg.2.168`, PR 222) complete: qualified, owner cleared, merged into engine `31d72e1a` and delivered in X1 v2.3.9, Metroid v0.3.19 and SMW v0.15.3. Original measured package engine `8323d781`, MMX adapter `4fe87d3`, Metroid adapter `64ca06d`, SMW `324dfc8`. All four Magic Carpet cases, affected render/effect/temporal/current-state/rollback gates and unpaused SMW current-build load pass. Five balanced pairs per stock/wide profile through the capacity pool: all six CPU profiles neutral/inconclusive, no demonstrated speedup. Actual BSS -1,572,864 bytes MMX/SMW, -1,778,304 Metroid; median paired peak commit about 1.51-2.04 MiB lower. Metroid EndFrame stack 412,352 -> 1,456 bytes, Draw 214,432 -> 13,040. [Contracts/costs](RASTER_REGISTERS.md); [export lifetime](SCENE_EXPORT_MEMORY.md). The owner explicitly defers further Xbox/linked 32-bit qualification and dedicated memory-budget work. These shipped savings remain a byproduct of useful engine work; compressed rewind remains rejected for CPU regressions. |
| R6 | **Audit complete; title adoption outside this engine queue.** Existing `beads-8wg.2.115` records that Doom v0.04 pins divergent engine `838eb4c4` without the accepted 8bpp selector. Required input/replay callers prevent a blind pin update. The shared 8bpp optimization and historical qualification remain valid; reconciling the title fork is not a shared-engine completion gate. No new title implementation, runtime PR or gain claim. Evidence: `doom-adoption-20261028/evidence/final-disposition.json`. |
| R7 | Partial qualification recorded under `beads-qhnd`, which remains open for ordinary/reference full-race and listening checks. Five balanced matched stock5001-frame pairs show existing unchanged HLE median processCPU reduction12.79% (all pairs11.06-14.96%); no new optimization, GPU/presentation or Xbox claim. HLEv2 coordinate-assisted five laps/finish/results/next-race passed with game-owned flags. The same LLEv2 route and one bounded heading-reset v3 did not finish; unresolved fixture/backend interaction, not an established production defect or HLE improvement. Stop fixture tuning; HLEv3 not run. Prior source-identical repeat/state contracts reused. Normal desktop comparators and paired ordinary-route audio clips are staged outside delivery; no runtime PR/default change/release. Raw records, limits and handoff: `smk-dsp1-finish-20261028/evidence/final-disposition.json`. |

### Active queue and scope

R8's remaining shared-engine questions are open in the full native/HLE burndown.
Use existing evidence first, then test concrete candidates with up to three
owner-authorized item agents, keeping each item's work separate. Low old
sample shares alone do not reject all execution, DMA or audio optimization.
R7's existing shared DSP-1 consumer checks remain pending: ordinary HLE/LLE
races through results and the next race, listening, visual/current-build save/load.
Reuse the prepared builds and individual observations; do not tune the failed
assisted-race fixture or repeat cleared X1/SMW/Metroid owner checks. Completing
R7 alone does not finish the reopened engine investigation.

Owner direction on 2026-10-29: remaining items produce commits/PRs and QA
builds; publish only at the campaign endpoint, not after individual items.
Already published releases remain available. No new runtime change or release
is currently proposed for R7.

R9 follow-up disposition: exact independent compressed rewind storage was
rejected under `beads-8wg.2.170`; no runtime PR/adoption/release. Acceleration 1
increased MMX CPU by paired medians 4.29% stock/4.09% wide (5/5 slower each).
The second/final acceleration-64 variant was mixed/neutral in MMX and Metroid
stock, but Metroid wide increased CPU by 2.33% initially and 4.16% in a bounded
confirmation (5/5 slower). Confirmation saved 13.61 MiB ring+scratch capacity
and 5.43 MiB actual process peak commit; memory does not offset CPU regression.
Stop tuning after the two failed experiments. RAW remains production. MMX and
Metroid are actual shared-ring consumers; frozen SMW `324dfc8` does not call
that API, so its 23.08 MiB R8 capacity formula was hypothetical, not live
allocation or a promised saving. Do not migrate its host as part of this
shared storage item. Exact-loader/continuation probes and portable RAW/LZ4 tests
passed for acceleration 1; acceleration-64 unit tests passed, but further
fast64 actual-game rewind/portable tests stopped at the failed CPU gate.
SMW forced-unused-LZ4 stock/wide controls increased median CPU by 2.27%/0.73%;
stock is not cleared as neutral. No saving/pass is attributed to its unused
rewind API. Retain the rejected branch and raw evidence. Reopen only with a
demonstrated memory-limited consumer, a materially different design preserving
the same contract, and matched per-game CPU neutrality/improvement plus process
memory savings. The rejected experiment record and raw costs are indexed by owning issue
`beads-8wg.2.170`; no compressed backend is adopted in this branch.
Further Xbox/linked 32-bit and dedicated memory-budget work is deferred by the
owner; unavailable hardware is not a prerequisite for this campaign.

R6's title audit is complete; new Doom adoption is outside this engine queue.
SMK is a validation consumer of an existing shared DSP-1 implementation, not a
new title optimization. Substantial X2/X3 ports are also outside this queue.
The full native/HLE inventory defines the remaining shared-engine work.

The brief X2/X3 assessment is complete: current X2 master `9c3b374c` and X3
master `e88e5aab` both use the shared live PPU plus a shared Mmx23 margin
provider, rather than MMX's custom renderer. The X1 span build flag is not a
drop-in adoption. It embeds X1 scene/WRAM/private-CHR/palette rules. Reject direct flag adoption. A substantial title port is outside this queue;
any future reusable mechanism must have a measured shared consumer, with title
policies kept in adapters. Their existing correctness work and differing engine pins are
untouched. No new X2/X3 gain is claimed. Source hashes/extracts and disposition
are in `native-center-reuse-20261028/evidence/x2-x3-adoption-assessment.json`.

## Owner correctness follow-ups

The final Metroid candidate also includes the minimap-border presentation fix
in [title PR 44](https://github.com/mstan/SuperMetroidRecomp/pull/44), source
`907fada`. It moves the final BG3 UI texel row with the existing HUD groups;
world coordinates and optimization/default choices are unchanged. Portable,
native/planned renderer suites, 28 actual capture/aspect comparisons, production
runtime and current-build overlay/fresh-process state checks pass. The combined
Metroid owner gameplay/rewind handoff passed on 2026-10-29; PR 45 integrated
the fix and stable v0.3.19 contains it. PR 44 was closed as superseded. X1 and
SMW passes remain cleared. Tracking: `beads-8wg.6.25` (closed).

The tested simple room-edge camera clamps are rejected under `beads-8wg.6.39`.
Two minimal clamp policies produce discontinuous offsets over an actual native
door sequence: one moves unchanged terrain on entry, the other moves a stationary
departing-door edge when the destination camera initializes. These are offline
policy projections over actual 342-wide images, not an implemented renderer
regression or proof that transition-aware anchoring is impossible. Camera
behavior stays unchanged. A future coherent asymmetric viewport must preserve
door coordinate ownership, HUD/windows/effects, native-center reuse, authored
scroll bounds and visible entity activity/netplay profile contracts before
new actual no-snap qualification. No camera runtime PR is warranted.

## Integration and delivery checkpoint (2026-10-29)

- Framework PRs [221](https://github.com/RetroPortingToolKit/snesrecomp/pull/221)
  and [222](https://github.com/RetroPortingToolKit/snesrecomp/pull/222) merged
  as `56e40c5f` and `31d72e1a`. The latter tree equals qualified `e923610a`;
  its portability follow-up does not change guest or hot-frame behavior.
- [X1 v2.3.9](https://github.com/mstan/MegaManXSNESRecomp/releases/tag/v2.3.9)
  is delivered and owner cleared. No additional release is needed for the
  engine's cold configuration-string portability fix.
- Metroid [PR 45](https://github.com/mstan/SuperMetroidRecomp/pull/45) merged
  the owner-approved current-main bundle as `cd65e72e`; release
  [PR 46](https://github.com/mstan/SuperMetroidRecomp/pull/46) merged as
  `459ae36e`. Stable [v0.3.19](https://github.com/mstan/SuperMetroidRecomp/releases/tag/v0.3.19)
  passed all four platform CI gates. The actual Windows CI artifact also
  passed muted runtime, overlay/save-load and fresh-process continuation checks.
- SMW [PR 25](https://github.com/mstan/SuperMarioWorldRecomp/pull/25) merged
  as `5210523d`, pinning engine `31d72e1a`.
  [v0.15.3](https://github.com/mstan/SuperMarioWorldRecomp/releases/tag/v0.15.3)
  retains the stock and legacy IPS co-op executables in one Windows ZIP, plus
  the stock Linux AppImage. Actual packaged Windows and native Steam Deck
  checks passed, including current-build save/load and Lua/helper where
  applicable. The optional paired 600-tick legacy LAN gate did not pass:
  slot 0 reached 600 and exited while slot 1 was at 594. This teardown limit
  is recorded, without a full netplay or cross-build compatibility claim.
- Each title has one verified current Windows ZIP in the SNES delivery folder.
  Superseded ZIPs were removed after replacement verification; native saves
  and owner settings were preserved. New source optimization switches remain
  OFF and the custom renderer remains elective; qualified Windows release
  flags explicitly enable the accepted paths. Other supported build floors
  remain available.

The final artifact hashes, source identities and delivery records are indexed
by `current-main-integration-20261029/evidence/metroid-closeout.json` and
`smw-engine-release-20261029/evidence/closeout.json` under the retained local
task parent. They supplement the original measured packages; release artifact
correctness does not transfer old performance percentages to another compiler
or binary. Finished isolated Linux/Deck build scratch has been removed after
importing and hashing its raw evidence. Current-build QA profiles and needed
campaign samples remain retained; disposable local build objects and duplicate staging were removed with their
cleanup manifests preserved.

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
7. After two unsuccessful improvement experiments, reassess and pivot. Reject
   concrete proposals that do not demonstrate useful value under their contract. GPU, coprocessor and
   driver replacements remain conditional until their workloads justify them.
8. For new changes requiring human regression coverage, bundle compatible
   candidates and pass automated gates first, then prepare normal-window,
   normal-audio owner checks. Preserve observations and wait for the owner's
   approval before merging that new batch. Do not repeat cleared checks.

## Prior completed and rejected work

Diagnostic culling, cached frame composition, exact raster snapshot reuse,
deferred reference pixels, APU event spans, native Mode 1/OAM/window jobs,
slicing CRC and MMX whole-view row composition are baseline work. The earlier
14-opportunity campaign is dispositioned in `beads-8wg.2.154`; reuse its evidence.

Two palette/color-cache experiments and the earlier generic composition-helper
and CPU bus-guard candidates were rejected. Reopen them only with new evidence.
Remaining shared execution/bus/DMA, audio and scheduling/coprocessor questions
are explicitly open under R8. New title-specific AOT roots or music-driver
hooks are outside this queue. Historical component shares are not current
post-row ceilings and cannot establish that no further useful gains remain.

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
The completed shared-memory package is indexed by
`shared-render-registers-20261028/campaign-results.json`, including raw per-game
samples, exact package labels, host-pool receipts and image/commit costs. No
aggregate CPU gain is claimed from the small mixed per-profile results.
