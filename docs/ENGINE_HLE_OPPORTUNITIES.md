# Engine acceleration assessment

This is a shared-engine assessment, not a search limited to guest function
hooks. Follow the owner's [HLE policy](https://github.com/mstan/recomp-ai-rules/blob/main/HLE.md):
keep the working LLE reference, select implementations when building, and
qualify the caller contract and useful host savings. Different private state,
algorithms and internal timing are permitted. A title hook is one possible
boundary, not the definition of HLE.

The rows below distinguish candidates from measured improvements. Existing
optimizations cannot be credited again as new gains. No mobile or original
Xbox performance has been measured here.

| Route | Maintained floor and useful replacement boundary | Evidence and disposition |
| --- | --- | --- |
| Graphics: ordinary tiled backgrounds | `ppu.c` scanline/window/priority contract; decode a source tile span once rather than once per output pixel | Ordinary 2/4bpp paths already batch tiles. 8bpp still repeats map and plane resolution per pixel. This branch implements that bounded portable candidate. Whole-title share remains unknown. |
| Graphics: large tiles, offset-per-tile, mosaic | Complete layer spans with their scroll overrides, palette, transparency and clipping; keep caller-visible raster events | Scalar work remains, but each has different boundary rules. These are separate candidates, not rejected by the 8bpp experiment. Existing Mode 2 capture and widescreen shadow consumers must be included in the contract. |
| Graphics: Mode 7 and full presentation | Consume raster-state/VRAM snapshots and emit native/HD images; affine spans, cached texels or a host GPU renderer | F-Zero's separate custom renderer already reports substantial renderer savings; those are title/presentation results, not fresh generic-core gains. A generic GPU path needs raster changes, windows, main/subscreen color math, readback and fallback scope specified. Mode 7/DSP-1 also warrants actual hot-route attribution. |
| Graphics: sprite evaluation/composition | Line result plus status bits consumed by the guest; independent presentation can use a different internal algorithm | Prior generic composition helper refactor regressed Mega Man X by about 3.8% and was rejected (`PERFORMANCE.md`). Do not repeat that refactor or infer that all composition alternatives are exhausted. |
| Audio: SPC700 execution | Audio command/port service or a larger known music-driver transaction; LLE SPC700 remains the floor | Recompiling hot blocks is an exact execution optimization; replacing a whole driver is HLE but often title-specific. Main CPU ports, RAM upload/readback, sample boundaries and timer polling define observability. Current component cost for a new candidate is unknown. |
| Audio: DSP synthesis/resampling | DSP register/RAM input to PCM plus observable DSP state; vector voice processing, BRR reuse and block mixing | Separate SPC cost from DSP synthesis and host output. Echo RAM, pitch modulation, key-on, live BRR writes and mute semantics constrain batching. Prior lazy audio cache wins are already baseline. No new audio speedup is asserted. |
| DMA and memory | Whole supported transfer spans with bus latch, register completion and interrupt/beam consequences | Contiguous WRAM/VRAM bulk paths can avoid repeated dispatch, but MMIO, wrap, HDMA raster effects and observers require explicit supported families. Direct address decoding and cached mappings may simply improve exact LLE. No current measured ceiling for a new SNES DMA candidate. |
| Scheduling and idle waits | Run to externally relevant CPU/APU/IRQ/HDMA events; service known wait loops directly | Potentially larger than leaf math. Beam timing has real IRQ/HDMA dependencies documented in `snes.h`; this is not a requirement to reproduce unobserved internal cycles. Establish event ownership and a replayable busy-wait workload before replacing the scheduler. Unmeasured. |
| Coprocessors and firmware | Command/job interface for DSP-1, Cx4, Super FX or SA-1, retaining each functioning low-level implementation | DSP-1 already has the separate draft experiment below. Super FX render jobs and Cx4 vector commands are broader candidates; software-defined job recognition may be title-specific even inside a shared engine. Their output buffers, status/IRQ/readback and guest consumers need qualification. SNES has no universal game BIOS API to replace. |
| Guest libraries/assets | Recognized decompression, math, memory or graphics submission transactions | Useful when hot and stable, but title hooks are complementary to the engine routes above. A low-cost leaf is not a substitute for profiling renderer/audio/scheduler work. |

Related DSP-1 work: [framework draft](https://github.com/RetroPortingToolKit/snesrecomp/pull/150)
and [Super Mario Kart qualification draft](https://github.com/mstan/SuperMarioKartRecompiled/pull/1).
Its noisy desktop timing is promising, but visual/gameplay qualification is
unfinished. This branch neither promotes that backend nor counts its gain as
an 8bpp result.

## Portable 8bpp span experiment

Configure a game using `runner/runner.cmake` with
`-DSNESRECOMP_PPU_8BPP_IMPL=HLE`; `LLE` is the default. Direct compiler users
can define `SNESRECOMP_PPU_8BPP_HLE=1`. Invalid selector values fail configuration.
No runtime toggle or dual execution is introduced.

The replacement consumes the same PPU state and emits the same priority/color
indices for the existing 8bpp layer call. It resolves tilemap, character and
four plane words once per span, ending at an eight-pixel source boundary or
window edge. A 16x16 tile's midpoint selects a new character. Flip direction,
VRAM wrapping, scroll phase, transparent pixels and priority comparison remain
part of the contract. All batching state is automatic local storage, so no
persistent state, ABI, savestate identity or public interface changes.

This is an **exact batching optimization of an existing shared renderer**,
exposed through the build-selected implementation experiment. It is not a
new whole-PPU behavioral model, and the label HLE does not by itself imply a
performance win. Larger renderer replacements remain eligible under policy.

`tests/ppu/run_8bpp_contract.py` builds the original `88a9f7f` renderer, current
LLE and HLE separately. Known plane/flip examples and 4,096 deterministic
random cases cover 8/16-pixel tiles, both screens, all tilemap page layouts,
scroll/VRAM wrapping, windows, existing priorities and negative widescreen
coordinates. All three produced `f156f6e7c01040c5`. The test supplies synthetic
state: it is not an independent hardware oracle or a gameplay/softlock test.

Reproduce with native executables on Windows (absolute paths avoid MSYS shell
path rewriting):

```text
py -3 tests/ppu/run_8bpp_contract.py --cc C:/msys64/mingw64/bin/gcc.exe --git "C:/Program Files/Git/cmd/git.exe"
```

Add `--bench` only during an isolated serial timing window. The harness warms
each build, alternates six LLE/HLE pairs, checks output hashes and writes
`build-ppu-hle/measurement.json`. It times the synthetic layer renderer and
destination clearing, excluding fixture generation and digest computation.
No whole-game improvement or default promotion follows from that measurement.

The 2026-10-06 GCC 15.2.0 `-O3` Windows x64 screen on a Ryzen 7 9800X3D used affinity mask 4,
2,000 synthetic frames of 224 lines per arm, one warmup each, then six
alternating pairs. Reported component intervals were:

| Pair | Order | Scalar ms | Batched ms | Time reduction |
| --- | --- | ---: | ---: | ---: |
| 1 | Scalar, batched | 554 | 282 | 49.10% |
| 2 | Batched, scalar | 568 | 320 | 43.66% |
| 3 | Scalar, batched | 534 | 289 | 45.88% |
| 4 | Batched, scalar | 539 | 286 | 46.94% |
| 5 | Scalar, batched | 588 | 319 | 45.75% |
| 6 | Batched, scalar | 615 | 347 | 43.58% |

Median paired component time reduction was **45.81%**; raw medians were
561 ms and 304 ms. Every arm returned output hash `53d95e1d38490d4a`.
The original source/current scalar/batched contract check was rerun immediately
before timing and matched. Team benchmarks and builds were serialized; process
snapshots were taken only at screen boundaries, so they cannot establish an
uncontaminated host throughout the run. Foreign compiler/game processes were
observed, including an active Super Mario World process. The standalone test
executables were 384,542 bytes scalar and 384,030 bytes batched; these are test
binary sizes, not linked game sizes. Treat this as a component screen,
not a precise isolated gain estimate or a game FPS result.

The experiment remains **draft and default-off**. Required next evidence is
an actual useful 8bpp workload's share of total host cost, gameplay and transition
qualification, and whole-title paired timing. SMW/MMX Mode 1 or SMK Mode 7
timings would not demonstrate a benefit from this changed path. No such
whole-title claim or softlock qualification is made here.

Tracking: central Beads `beads-hr1g`, under the SNES framework epic.

## Representative-workload discovery (2026-10-06)

Use a three-title coverage pool: Mega Man X (ordinary tiled Mode 1), Super
Mario Kart (Mode 7/DSP-1), and Doom (Mode 3/8bpp plus Super FX). This is not an
automatic matrix. Reuse evidence first, with one production configuration per
selected workload; the complete cross-system discovery pass permits at most
six new captures total. Neither this pool nor a successful sample establishes
exhaustive hardware, audio, game or enhancement coverage.

Reused evidence:

- `F:/Projects/snesrecomp/snesrecomp/build/perf/final_phaseon_20260905/summary.json`:
  September 5 SDL3 300-frame startup smoke, SMALL history and audio-render calls
  zero. MMX inclusive guest/PPU/present times are 118.228/24.374/161.586 ms;
  SMW's are 81.215/58.446/145.893 ms. These stale presentation-heavy startup
  measurements do not establish current active Mode 1 or audio priorities.
- `F:/Projects/snesrecomp/_wt-smk-dsp1-qualification/build-lle/paired-performance/five-pairs.json`
  and its `docs/DSP1_QUALIFICATION.md`: October 6 GCC 15.2 Release, 5,001-frame
  driving route, median paired DSP-1 HLE throughput delta +10.9536%. Foreign
  contention and unresolved visual/audio acceptance prevent qualification;
  this is not subsystem attribution or an 8bpp gain.
- `F:/Projects/snesrecomp/DoomSNESRecomp/build-validation/baseline-final-a/stderr.log`:
  October 6 active window frames 1358-2056 (699 presentations): guest
  2208.631 ms, raster capture 297.424 ms, composition 298.837 ms,
  upload/present 313.839 ms, and state trace 148.652 ms. That diagnostic route
  includes trace/dump overhead; it cannot independently isolate GSU or 8bpp.
- Historical context, outside the selected three-title pool:
  `FZeroRecomp/docs/HD_MODE7_PERFORMANCE.md` (September 21), recorded BS Deluxe
  race frames 1600/1601, GCC -O3, custom-renderer medians 3.18-13.92 ms across
  output settings. Emulation, audio, upload and display are excluded; these
  already-realized presentation savings are not generic-core gains.

One fresh bounded Doom discovery sample is preserved at
`build/profile-subsample-20261006/doom/`: `samples.csv`, `attribution.json`,
`samples.csv.child.log`, and `route.txt`. The production baseline executable
SHA-256 is `02B769001365B1BFAD889EC881F9897A462E5B35C0B00F7188987AD0734F6CEC`.
It completed 2,056 frames with child exit 0 and zero sampler errors, audio
output off and diagnostics absent. This is the entire boot-to-gameplay route,
not an isolated active-game window or a throughput benchmark.

Of 457 samples, 194 (42.451%) are outside the executable; nearest-symbol self
bins include `_interp_run_core` 10.284% (47 samples), `instruction.constprop.0` 6.783%,
`bridge_bus_read` 4.595%, `run_one` 3.720%, `read_opcode` 2.845%, and
`ppu_runLine` 2.407%. Main-thread instruction pointers are not call stacks:
these bins cannot supply inclusive subsystem totals, identify every inlined
helper, or cover worker/audio-device costs. Outside-module samples are not
attributed to an engine service. Sampling perturbs execution and can alias
periodic work; no speedup follows from it.

Doom does reach the shared 8bpp renderer: its `GameDrawPpuFrame` calls the beam
renderer, which invokes `ppu_runLine` before the line observer, and Mode 3 BG1
calls `PpuDrawBackground_8bpp`. `DoomRendererPreparePpu` binds an OBJ overlay;
the optional custom compositor does not bypass stock scanout. This source
reachability is not an independent measurement of 8bpp cost. The existing
active-window phases and new self samples support investigating a broader
guest/GSU/bus service boundary before prioritizing 8bpp alone. That is a theory
for the next bounded investigation, not an HLE qualification or promotion.
The component experiment and default-off disposition above remain unchanged.

## Execution strategy and completion gate

The first new SNES work is the missing active Mode 1 production cost screen in
Mega Man X. The September startup smoke cannot select another PPU helper.
Reuse SMK's current route and Doom's fresh production sample; take a new screen
only when the missing active evidence can change the implementation boundary.
Use one production configuration per title and at most one missing active
attribution capture per selected game for this implementation round. The earlier
six-launch cap bounded the completed discovery pass; it is not a lifetime cap.

| Workload | Bounded useful route and production-floor milestone | Role |
| --- | --- | --- |
| Mega Man X | Stock intro-highway movement, jump/shoot, enemies and one death/retry or stage-exit transition. Record the actual active event/frame window and a short replay once. Refresh the stale September floor with current build/ROM identities and production diagnostics off. | Planned primary ordinary tiled renderer/composition service. Current active Mode 1 cost and current replay readiness are gaps; do not assume the startup percentages persist. |
| Super Mario Kart | Reuse `tools/smk-one-player-race.txt` and the retained 5,001-frame driving/pause/GIVE UP-to-RETRY/END route from `_wt-smk-dsp1-qualification`. Its observed transition is the bounded gameplay gate; no mandatory full cup. | Mode 7/DSP-1 companion. Use maintained LLE firmware for a renderer comparison; the separate DSP-1 draft's unresolved visual/audio acceptance remains separate. |
| Doom | Reuse `build/profile-subsample-20261006/doom/route.txt`: E1M1 entry around frame 1358, forward movement, turn/fire, pause/resume, exit at frame 2056. Baseline-final-a production SHA and native disabled-enhancement settings are recorded above. | Mode 3/8bpp/Super FX companion, or primary if the measured broader guest/GSU boundary wins selection. Audio output was off in the sample: audio/device and worker cost remain unmeasured. |

Choose the first implementation only after the active MMX screen resolves the
ordinary-renderer gap. The planned graphics boundary is the shared layer-span
and main/subscreen composition service, including windows, priority, sprites
and color math, rather than one more scalar helper. It may use native spans,
cached decoded input or a different private renderer while preserving raster
updates and caller output. If its recoverable cost is not useful, select the
Doom-supported broader guest/GSU/bus job service instead: output buffers,
completion/status/IRQ and CPU/APU-visible memory effects define that boundary.
The fresh Doom self bins do not by themselves identify a safe whole-job HLE or
justify deleting GSU execution. Document the concrete supported job/scene family
and measured eligible share before coding. No implementation is chosen merely
because a title contains an 8bpp call or a coprocessor.
If a Super FX service becomes the selected candidate, replace the Mode 1/Mode 7
companions with runnable Star Fox and Yoshi's Island routes that exercise that
service. Verify their functioning floors first; MMX/SMK cannot supply Super FX
qualification. This is a replacement three-game set, not six extra benchmark
legs. If that narrower service is only supported by Doom, state a Doom-only
pilot and do not promote it as a multi-game Super FX result.

Before coding, declare the primary affected active window and useful gain
metric. The preliminary planning target is 10% less whole active-workload
CPU/frame work at identical useful guest progress and presentation settings,
without worse pacing/tail behavior. This is not a universal acceptance floor
or a promised outcome: the candidate must exceed observed noise and deliver a
useful gain on its tested platform. Report all-thread process CPU, wall/frame
work, pacing tails and eligible-work/dispatch coverage; separate attribution
captures from uninstrumented timing and exclude unrelated presentation wins.

For a renderer replacement, compare raster-state/VRAM/HDMA inputs and native
frame output through palette changes, windows, sprite priority, transparency,
main/subscreen color math, blanking and transition scenes. Preserve guest
register/status, DMA and IRQ/NMI consumers. For a GSU job replacement, compare
written buffers, readback and completion/IRQ consumers plus the actual resulting
scene. Compare same-build save/load continuation if private state changes;
reject unsupported cross-build state before mutation. Preserve the real LLE
build. Both builds run the bounded primary and two companion routes with
retained checkpoints, state/event evidence and relevant short audio evidence.
Minute policy-permitted differences need practical assessment, not automatic
rejection or silent acceptance. No stalled guest progression or softlock passes.

Timing is bounded: two balanced primary pairs (ABBA), and one A/B per companion,
with one production configuration each. Use exact-build correctness outputs
where reusable, but separate performance-only logs from trace/hash/dump runs.
Record cost, code size, coverage and contamination. Do not automatically repeat
a noisy result, build a same-binary matrix or qualify every game. Inconclusive
or unsuccessful candidates remain drafts with their evidence.

The planned final owner handoff is the playable Windows x64
`MegaManXSNESRecomp.exe`, stock presentation, built with the selected shared
renderer and accompanied by exact config/build/ROM identity and the short
highway route. The owner plays normal movement, jumps, shooting, retry and
controls/audio feel. If cost screening instead selects the GSU service, replace
that primary handoff explicitly with `DoomSNESRecomp.exe` and its stock E1M1
movement/turn/fire/pause/resume route; do not claim an MMX renderer win from a
Doom result. Only a promising measured implementation reaches subjective
playtesting. After owner acceptance, make HLE default solely for the qualified
platform and documented supported service scope, keep build-time LLE opt-out,
merge and close the issue. Rejected behavior, feel or insufficient gain leaves
LLE default and the experiment draft. Neither full campaigns nor an endless
all-games qualification matrix are prerequisites for this scoped completion.

## Measurement, decision and delivery protocol

Owner completion rule: establish a material game-workload gain and automated
compatibility, then deliver the final playable build for the owner's feel check.
After that check passes, integrate the prepared default change and close the
scoped work. Exhaustive game coverage and completed campaigns are not additional
completion requirements.

1. **Pin the workload and floor.** Use the three games and concrete routes above.
   Build LLE and HLE from the same title/framework revisions, compiler/options,
   ROM/firmware identities, presentation/audio settings and initial game state;
   only the selected implementation differs. Keep the replaced LLE service
   runnable. An old executable is discovery evidence, not a mismatched control.
   Use native game saves or replayed inputs when private savestates cannot cross
   builds. First resolve the named route/build gaps; do not perfect unrelated
   hardware before replacing a functioning operation.
   Verify that companion routes actually exercise the replacement; an unaffected
   title is a regression control, not evidence for that HLE service. If the
   chosen service changes, replace an unsuitable companion in the three-title
   set instead of accumulating extra games or claiming unexercised coverage.
2. **Attribute only what is missing.** Reuse suitable profiles and collect at
   most one new active-workload attribution capture per selected game in this
   implementation round. Identify the intended service's eligible dynamic work.
   Include worker threads and external modules or report them unresolved; a
   main-thread symbol histogram cannot supply a whole-process cost percentage.
   Capture diagnostics separately from performance. End discovery when there
   is enough evidence to select a useful service, not when every subsystem has
   a profile. The earlier six-launch discovery cap applied to that completed
   pass, not to the whole implementation/qualification program.
3. **Choose one replacement.** Record its caller ABI, inputs, outputs, observable
   side effects, supported operation scope, permitted tiny differences, expected
   cost removed, and candidate-specific useful gain before coding. Implement a
   shared service with build-time LLE/HLE selection and explicit build identity.
   Do not stack several speculative replacements into the same comparison.
4. **Measure equivalent active play.** Delimit a fixed gameplay window by guest
   frames and meaningful game events, excluding boot, warmup and teardown.
   Choose enough active work to dominate measurement granularity once, then keep
   it fixed. Report total process CPU milliseconds per guest frame (all threads),
   critical-path frame work, median/p95 frame time and missed presentation/audio
   deadlines where available. Record peak memory and code size, since constrained
   targets matter. Preserve normal renderer and audio production; a benchmark
   that omits presentation/audio is a core-only diagnostic, not end-to-end proof.
   The owner selected Windows first and authorized uncapping for useful
   measurements. Prefer a finite uncapped comparison where it preserves the
   same game, render and audio-synthesis work. Remove host frame-delay/VSync
   waits only in isolated benchmark configuration; do not change the guest
   timing model, resolution, effects, audio workload or HLE coverage between
   builds. Report uncapped FPS and milliseconds/frame alongside total CPU/frame,
   and verify completed render/audio work and game progress rather than trusting
   a frame counter alone. A legacy benchmark that skips rendering/presentation
   or audio remains core-only evidence; use a complete paced CPU/frame comparison
   until that benchmark path can exercise equivalent work. Normal capped play
   can show reduced CPU/frame even when FPS stays unchanged. Measure GPU
   completion/queue cost when work moves there; a shorter submission call alone
   is not a win. Keep the final owner-playtest package normally paced.
5. **Use a fixed comparison budget.** The primary game gets LLE/HLE/HLE/LLE:
   two order-balanced pairs, four measured executions. Each of the two companion
   games gets one LLE/HLE pair, two executions each. That is eight measured runs
   per candidate on one declared host/configuration, not a Cartesian matrix.
   Reuse their progression telemetry and final outputs; take expensive milestone
   captures outside timing, and use isolated LLE/HLE fixtures for detailed
   contracts. Do not automatically add separate full campaigns or trace runs.
   Keep team builds/profiling out of the timed window, record host load/power/
   thermal conditions, and preserve every result. A noisy or contradictory result
   stops that screen; fix an identified condition before a bounded replacement
   measurement. Never repeat until a passing subset appears.
6. **Decide from useful gain and compatibility.** Report both paired percentage
   and absolute savings, with the observed pair spread. About 10% lower whole
   active-workload CPU time is a planning aim, not a universal acceptance rule.
   A candidate may instead solve a declared frame-budget or stutter problem.
   Both primary pairs must show a clear consistent useful improvement beyond
   observed noise; two pairs are not a formal confidence interval. Companion
   single pairs screen for large regressions, not proof of zero performance
   change. Explain any apparent regression before broadening defaults. Exact
   promises require exact outputs; permitted approximations use a declared
   practical image/audio/result comparison. Check input, audio, progression,
   affected completion/IRQ consumers, transitions and relevant pause/reset/save
   behavior. No crash, softlock, stale buffer, lost completion or save corruption
   passes. A huge isolated kernel ratio cannot substitute for this decision.
7. **Hand off the actual finished candidate.** Provide the named primary game as
   a ready-to-launch normal-paced HLE package, an LLE comparison build, isolated
   save/checkpoint setup, launch instructions and checksums/build identity. Include
   a short before/after report, companion results and any tiny known differences.
   Prepare the intended default-selection/integration change in the draft PR so
   the owner tests the package intended to ship. Ask the owner to play normally
   and assess response, motion/collision, camera/scrolling, stereo where relevant,
   audio rhythm and continued progression. There is no prescribed full-campaign
   completion or multi-game human test matrix. Owner rejection reopens the
   affected behavior; fix and recheck that change before another handoff.
8. **Finish the scoped delivery.** After owner acceptance, integrate the reviewed
   candidate, make HLE the default for the supported titles/platform/service,
   retain a documented build-time LLE opt-out, and record the measured and manual
   evidence before closing the issue. Do not add unrelated qualification gates
   after the agreed playtest. If the replacement cannot deliver material gain,
   preserve its branch and draft PR with results, explain why, and choose a new
   boundary deliberately; an unsuccessful experiment is not a completed system.

The owner selected **Windows first; port measurements later**. Windows x64 is
therefore the initial implementation, measurement, final-playtest and default
scope. After automated checks, material gain and the owner's normal-paced feel
approval, finish that Windows delivery; a mobile/Xbox port is not a new gate
before closure. Later port work carries the winning candidate and relevant
routes to the chosen target and measures there before claiming target savings.
Do not multiply all hosts into the Windows discovery/comparison matrix.
