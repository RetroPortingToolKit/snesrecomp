# Shared SNES native/HLE burndown

Current owner scope: 2026-10-29. Roadmap `beads-49cy`; coordinating campaign
`beads-2wy8.1` (`campaign:optimization-campaign`). This is the full campaign
inventory. [The nine follow-ups](OPTIMIZATION_BURNDOWN.md) supplement it; their
count is not a completion percentage for the whole native/HLE investigation.

Optimize shared engine mechanisms. Games supply workloads and validation;
new title-specific renderers, ROM hooks and pin adoption are outside this
queue. Previously authorized MMX/Metroid adapters are finished history.
Useful measurable gains qualify without requiring a dominant subsystem or a
large rewrite. Each tested game must improve or remain neutral; small mixed
results within variation are inconclusive, not wins.

Use **rejected** when a concrete proposal was infeasible under its contract
or did not demonstrate useful value. State the tested scope and reason;
absence from one workload is not proof of impossibility in every game.
Use **open** for unanswered questions. **Deferred** is reserved here for the
owner's explicitly postponed Xbox/platform and dedicated memory-budget work.
Retain already shipped memory improvements and rejected experiments as history.

## Original 14-item checklist

These names and original assessment are from closed issue `beads-8wg.2.154`.
Its accepted package and owner approval remain valid. The final column below
records what that evidence does and does not settle for the broader current
engine investigation. Old component samples are not current performance ceilings.

| # | Original opportunity | Original assessment | Current result / remaining work |
| --- | --- | --- | --- |
| 01 | APU event-span servicing | Retained | Shared implementation shipped with a portable reference. Do not count it again as a new gain. |
| 02 | Deferred reference raster materialization | Retained in MMX/Metroid | Lazy reference rendering shipped; guest-visible PPU status/readback and fallbacks preserved. |
| 03 | Mode 1 native tile rows | Retained | Shared pure jobs shipped; scene and asset policies stay in title adapters. Later row/span work is recorded separately. |
| 04 | OAM matching/indexing | Retained in MMX; blind Metroid port not justified | Accepted jobs/adapters shipped. Reject the blind port for the tested Metroid workload; other shared sprite costs belong to R8.1. |
| 05 | Window/color processing | MMX/window plan retained; Metroid neutral | Accepted shared plans shipped. The older generic composition-helper regression remains rejected. |
| 06 | Shared renderer jobs | Tile/OAM/color/window interfaces retained | Shipped. Reuse the shared interfaces for measured consumers; do not move title rules into the engine. |
| 07 | Immutable raster snapshot reuse | Already optimized | Existing equality/write-interval reuse is baseline, with invalidation and full-copy fallback. |
| 08 | Native memory / bulk DMA | Native arrays/copies already present; speculative rewrite set aside on about 0.9% sampled cost | Native access is baseline. Remaining shared bus/DMA work is **open under R8.2**; the old sample alone is not an optimization experiment or a universal rejection. |
| 09 | Mode 7 / 8bpp / mosaic | Reuse existing F-Zero renderer; no further general shortcut demonstrated | Shared 8bpp is implemented with historical Doom qualification. F-Zero's custom renderer is a title result. R5's unused-path proposal is rejected for the inspected workloads; remaining shared rendering/coprocessor questions are R8.1/R8.4. |
| 10 | Deeper audio driver replacement | Set aside on about 4.2% remaining sampled cost and no common music protocol | Reject a universal music-driver replacement without a shared protocol; no title driver hooks in this queue. Shared SPC execution/DSP synthesis costs are **open under R8.3**. |
| 11 | Further AOT / bridge work | Set aside on about 4.3% sampled cost, no interpreter cap | Remaining shared execution/bridge costs are **open under R8.2**. Do not require a large interpreter share or count newly generated title roots as an engine optimization. |
| 12 | Portable slicing-by-8 CRC | Retained | Shipped with independent-reference checks for seeds, alignment, tails and incremental updates. |
| 13 | Modern graphics upload formats | Already 32-bit output/direct BGRA upload | Reject conversion-removal work: that conversion is already absent. Cached host frame composition is also baseline. Any remaining shared upload/composition cost needs R8.1 evidence. |
| 14 | Palette / RGB conversion caches | Two failed experiments removed | **Rejected.** No useful built-game gain demonstrated; do not resume tuning the same designs. |

The broader [engine assessment](ENGINE_HLE_OPPORTUNITIES.md) also identifies
scheduling/idle waits and command-level coprocessor services. These are covered
explicitly by R8.4, rather than disappearing behind a completed table count.
Guest library/asset hooks remain complementary title work outside this queue.

## Remaining engine investigation: R8

Owning issue `beads-8wg.2.169` is reopened with this scope. Its previous X1
samples screened for a dominant whole-subsystem replacement, not every useful
shared optimization. Preserve those samples and their limitations; they do not
establish that remaining engine work is exhausted.

Owner update on 2026-10-29 permits up to three concurrent item agents, one
item per agent. Use existing evidence first, keep branches/evidence isolated,
and independently review each result before integration. Scheduling waits for
a slot while rendering, execution and audio proceed:

| Task | Question to resolve | Completion evidence |
| --- | --- | --- |
| R8.1 — shared rendering | What repeated work remains in shared tile rows, Mode 7/affine spans, sprite/window/color composition, full presentation and host frame upload after the shipped packages? Include shared software batching/caching and GPU rendering as evidence-selected alternatives, without presuming a rewrite. Existing stock X1 observations name `PpuDrawBackground_4bpp` and `PpuRunLineJob`, but do not supply inclusive totals. | Identify a specific operation and real consumer; compare a coherent shared implementation in matched built games, or reject the concrete proposal with measured/source evidence. Preserve raster/status/readback and fallback contracts. Do not repeat rejected palette caches, generic composition helper, or R3/R5 proposals on the same evidence; those rejections do not reject all Mode 7 or full-presentation alternatives. |
| R8.2 — execution, bus and DMA | Can shared CPU/interpreter bridge, address decoding or supported transfer spans avoid repeated work while preserving bus/status/IRQ/readback behavior? | Reuse prior bus-guard rejection. Establish a current concrete caller and cost before a new design; measure actual built-game CPU, keeping compiled execution and interpreter fallback valid. No new title-root campaign. |
| R8.3 — audio service | After event-span servicing, is avoidable cost in SPC execution, DSP synthesis/resampling or repeated synchronization? | Separate these costs before choosing a change. Qualify the service's port/timer/RAM/PCM and current-build state contract. No unsupported universal music-driver assumption and no title-specific driver replacement. |
| R8.4 — scheduling and coprocessors | Are there useful shared idle/event or command-level services in DSP-1, Cx4, Super FX or SA-1 that can replace repeated low-level work? | Use a functioning maintained reference and a supported actual consumer. Define completion, memory ownership, IRQ/readback and continuation behavior; implement/measure a candidate or reject a concrete proposal with its reason. Do not make Doom fork adoption or a new game port a prerequisite. |

These are bounded selection tasks, not promises of four implementations or
permission for a benchmark matrix. For each, record the specific proposals,
their measurements and accepted/rejected results. If existing evidence cannot
answer a question, take the smallest measurement that can. Neither a low old
sample share nor failure to find a major rewrite closes the question by itself.
After two failed improvement experiments, reassess before further tuning.

## Separate consumer validation and excluded work

- **SMK / shared DSP-1 validation (`beads-qhnd`): pending.** The implementation
  and build-selection work already exist in shared engine PR 150. SMK is its
  test consumer, not a new title-specific optimization. Finish ordinary HLE/LLE
  race, results, next-race, listening and current-build save/load checks. The
  assisted-race fixture is not ordinary gameplay acceptance; do not tune it
  again. Retain individual owner results and input/setup failures.
- **Doom title adoption (R6): outside this engine queue.** The 8bpp engine
  optimization and older qualification are retained. Updating the divergent
  game's fork and integration belongs to its existing title issue; it is not
  a blocker for completing shared-engine optimization work.
- **Xbox/32-bit platform and dedicated memory-budget work (R9): deferred by
  the owner.** Memory savings can accompany useful engine work, but no further
  standalone memory campaign is required here. Keep shipped compact registers
  and bounded export scratch. Compressed rewind remains rejected for CPU
  regressions; do not reopen it as part of this deferral.
- **R3/R5 tested proposals: rejected for their inspected workloads.** SMW
  main/sub BG requests had no same-line overlap; inspected Metroid captures
  did not enable the proposed special paths. No universal all-game claim.
- Direct X1 renderer-flag adoption in X2/X3 is rejected because the renderers
  and title policies differ. A substantial title port is outside this queue.
  The tested simple Metroid camera clamps are rejected for discontinuities;
  a new camera system is also outside this engine optimization queue.

## Validation and publication

Keep source optimization choices opt-in where already required, functioning
portable/reference implementations, current-build save/load/rewind/rollback,
and native owner saves/settings. No old save-state compatibility is required.
No cross-build or cross-host netplay claim follows from a successful build.

Use the capacity-reserved target pool and retain matched per-game raw samples,
source/binary/settings/host identities and optimization package labels. Reuse
cleared owner checks; do not relaunch those games without a new relevant change.
Each owner-authorized validator reviews one item. Keep no more than three
subagents active, including validators. Successful work gets a PR; rejected
work gets its reason/evidence in Beads before proceeding to another item.

Commits, PRs and QA builds may accumulate. Publish only at the campaign endpoint
under the owner's 2026-10-29 direction; no per-item releases or blog publication.
Closeout requires the open engine questions and separate required consumer
checks to have recorded outcomes, with the explicit Xbox/memory exclusion
preserved. Do not describe the original 14 or nine follow-ups as a percentage
of all possible engine optimizations.
