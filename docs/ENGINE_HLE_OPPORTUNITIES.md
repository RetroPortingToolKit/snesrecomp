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

Tracking: central Beads `beads-hr1g`, under the SNES framework epic.
