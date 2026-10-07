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

Windows is the first delivery scope. Start with MMX (ordinary Mode 1), SMK
(Mode 7/DSP-1), and Doom (Mode 3/8bpp/Super FX). Keep a functioning LLE service,
the caller ABI, and fixed build-time selection. Small practical differences
are allowed; complete internal-state, pixel and waveform identity are not
universal gates. Historical completed exact tests above remain unchanged.

### First work and replacement boundary

Resolve active MMX production cost and route readiness first, reusing SMK's
route and Doom's fresh sample. Take only a profile that answers a missing
selection question. Pin current matched builds, compiler, ROM/firmware and
normal presentation/audio settings; old startup measurements or executable
presence do not qualify a new implementation.

The planned renderer boundary is the shared layer-span and main/subscreen
composition service, including windows, priority, sprites and color math,
chosen only if the active cost can yield a useful game gain. Do not arbitrarily
add another helper. If the evidence instead supports a broader guest/GSU/bus
job service, declare its supported jobs, memory outputs and completion/IRQ
consumers before coding. Doom's self samples do not independently identify a
safe whole-job replacement or prove an 8bpp gain.

| Game | Ready route / remaining gap | Delivery role |
| --- | --- | --- |
| MMX | Stock intro-highway movement/jump/shooting on a current production pin. Refresh the stale startup floor and record a short useful route. | Planned primary renderer and playable `MegaManXSNESRecomp.exe`. |
| SMK | Reuse `tools/smk-one-player-race.txt` and the retained 5,001-frame driving/pause/GIVE UP route; use functioning LLE DSP-1 firmware for a renderer comparison. | Mode 7 companion; separate DSP-1 draft acceptance remains separate. |
| Doom | Reuse `build/profile-subsample-20261006/doom/route.txt`: E1M1 movement/turn/fire/pause, 2,056 frames. Audio output was off in discovery; ready normal-play builds must restore normal settings. | Mode 3/Super FX companion, or primary if a GSU service is selected. |

A Super FX replacement changes the three-game set to Doom, Star Fox and
Yoshi's Island, after verifying their functioning floors and useful routes.
This replaces companions rather than adding a matrix; unaffected MMX/SMK are
not evidence for that service. A Doom-only job remains an explicitly narrow
pilot, not a multi-game Super FX success.

### Focused objective checks and gain

Reuse existing focused rendering or job-boundary tests. Preserve required
buffer ownership, memory effects, raster/HDMA consumers, status, interrupts
and completion order. Test same-build save continuation only when the change
makes it relevant; add only a concrete missing correctness case. Crashes,
softlocks, lost completion and save corruption are defects. Do not mandate
checkpoint sweeps, full-state/audio matching, automated transition campaigns
or completed games.

Start with one matched same-route LLE/HLE performance pair per selected game.
Uncapped Windows measurement is allowed; report FPS and percentage gain with
equal useful guest progress, settings and host contention. Preserve normal
render/audio production for normal-game claims, keeping capture diagnostics
out of timing. Declare a useful candidate-specific gain before coding; 10% is
a planning aim, not a universal threshold. Reverse the order only if noise or
contradiction blocks the decision, with no automatic repetition or configuration
matrix. Inconclusive or unsuccessful experiments remain drafts with evidence.

Make one representative current-image sanity inspection of the new service:
looks right, not garbled. No pixel-perfect old/new comparison is required.
Deeper visual debugging follows an actual defect or owner report, not a preset
image/audio audit program.

### Human validation and Windows completion

After focused checks and material measured gain, package normal-paced ready
HLE games for all three selected titles, LLE alternatives, build identities
and simple launch instructions. Launch each ready game for the owner, one at
a time or in a per-system batch, and ask whether it looks and plays right.
Do not ask for launch permission again or substitute old/unqualified binaries
for the final candidate. The owner judges ordinary movement, controls,
graphics, sound and continued play; this practical check is the final gate.

Positive feedback plus measured gain completes Windows delivery: merge,
default HLE only for the supported service/title scope, retain build-time
LLE opt-out, and close with measured and owner evidence. If a defect is
reported, fix and recheck that affected behavior without imposing a new
campaign/matrix. Mobile/Xbox qualification is later scoped work, not an
additional prerequisite after owner acceptance of the Windows build.

## Shared desktop frame candidate (2026-10-06, in progress)

The refreshed MMX floor regenerated 4,695 AOT variants and linked successfully.
A single 1,956-frame native SDL capture completed with audio enabled, but its
image showed the title screen: early Start inputs did not establish gameplay.
The corrected route now uses the title's existing 1,400-frame menu timing and
WRAM entry conditions. It has not yet been run; no active-game gain is claimed.
The capture's host timing placed 3,191 of 4,583 ms in final composition. Its
329 main-thread samples were 85.41% outside the executable, so it cannot name
an exact DLL hotspot or establish a CPU-only percentage.

A concrete shared host issue exists independently of the PPU pilot: after
drawing to a locked SDL texture, the host reads the whole frame into its frozen
overlay snapshot. Reading mapped/write-combined memory can be costly. The
`SNESRECOMP_FRAME_IMPL=LLE|HLE` candidate composes in cached host memory, keeps
draw/blend/snapshot/OSD order, and uploads each row once with write-only copies.
It changes the presentation service's private buffer placement, not guest code,
PPU device behavior or audio synthesis. The current blend path already stages
only when blending; HLE generalizes staging to all supported frame widths.
LLE remains the default and the separate 8bpp pilot is LLE in both arms.
The predeclared target is 10% whole-runtime work reduction, subject to actual
measurement and owner acceptance. End-of-loop FPS/process-CPU telemetry is
separate from opt-in per-stage profiling.

The relevant pool for this boundary is MMX, Doom and Super Metroid. SMK and
SMW currently own separate desktop loops and do not call this service; their
prepared builds cannot qualify it and receive no performance claim. Doom uses
its pinned cf01c55 framework with the same host patch; MMX and Super Metroid use
this experiment framework with freshly generated matching code. Super Metroid
needs the current host's UI API and explicit resolved-target argument on its
two legacy interpreter dispatch calls. Those are floor compatibility repairs,
not credited as renderer gains. Paired builds, active-route checks and owner
handoffs remain pending. No screenshot/state/campaign matrix is planned.
