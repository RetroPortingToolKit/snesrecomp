# HD Mode 7 reconstruction (beads-8wg.2.192)

## Reference set, frozen 2026-11-01

These are behavioral and mathematical references. No emulator code was imported
or translated. Related bsnes implementations are one family, not independent
confirmation. This list covers relevant implementations, not every emulator.

| Reference | Revision | Role |
| --- | --- | --- |
| [bsnes-hd HD renderer](https://github.com/DerKoun/bsnes-hd/blob/fc26b25ea236f0f877f0265d2a2c37dfd93dfde9/bsnes/sfc/ppu-fast/mode7hd.cpp) | fc26b25ea236f0f877f0265d2a2c37dfd93dfde9 | Corrected appearance: line groups, inverse-coefficient interpolation, centered samples, optional supersampling. |
| [bsnes HD renderer](https://github.com/bsnes-emu/bsnes/blob/78474e5d39f58872dc21aca18cf9eb583b147fe7/bsnes/sfc/ppu-fast/mode7hd.cpp) | 78474e5d39f58872dc21aca18cf9eb583b147fe7 | Upstream implementation of the same enhancement family. |
| [ares Mode 7](https://github.com/ares-emulator/ares/blob/17f29369c40ec24250638b87887e23971e725462/ares/sfc/ppu-performance/mode7.cpp) | 17f29369c40ec24250638b87887e23971e725462 | Native clipping/truncation, flips, outside-map behavior, EXTBG, mosaic and direct color. |
| [Mesen SNES PPU](https://github.com/SourMesen/Mesen2/blob/b9fa69ddc6d0a331fb103fdb5eef6904305703c2/Core/SNES/SnesPpu.cpp) | b9fa69ddc6d0a331fb103fdb5eef6904305703c2 | Independent PPU timing/register and background rendering reference. |
| [Snes9x PPU composition](https://github.com/snes9xgit/snes9x/blob/1bcc369e89f08243e0a462882fb1f3e42e51de3a/gfx.cpp) | 1bcc369e89f08243e0a462882fb1f3e42e51de3a | Independent raster-state capture and composition reference; also see tile.cpp. |

Useful specifications: [Fullsnes](https://problemkaputt.de/fullsnes.htm),
[Rendering the Screen](https://wiki.superfamicom.org/rendering-the-screen),
[bsnes-hd settings](https://github.com/DerKoun/bsnes-hd#settings).
[Snes9x timing issue 1101](https://github.com/snes9xgit/snes9x/issues/1101)
is a reminder that matching an emulator screenshot cannot alone prove hardware
register-latch accuracy.

## Findings

The prior shared path sampled each scanline's quantized affine matrix. F-Zero
linearly blended adjacent prepared origins/steps. Neither recovered the
continuous projective plane. Increasing resolution therefore preserved geometry
errors already present in the quantized tables. Both also sampled from a native
pixel corner rather than around its sample position.

The standalone engine at investigation time was 8dc869cf; F-Zero 036a748 used
engine 0e81da50. This replacement is based on the latter GPU-capable engine.
Old standalone documentation described an earlier CPU-only implementation.

## New contract and mathematics

`kSnesHdReconstructed` jobs publish raw signed matrix/centre/scroll values,
original raster Y, composition state and a camera identity. Sealing the owned
frame fits `1/coefficient = intercept + slope*Y` using interior scanlines.
Constant and zero coefficients remain constant. Fits must explain the interior rows within quantization/table tolerance and
remain finite and bounded throughout every subrow, including extrapolated edges.
Changing camera/centre/scroll, flips, composition or discontinuous coefficients
separates regions. Failed fits use local affine geometry.

For output subrow `s` at scale `S`, geometry is evaluated at `Y+s/S-0.5`;
horizontal coordinates use `X+s/S-0.5`. Coefficients reconstruct the transform
before its origin is calculated. Signed hardware offset clipping remains;
intermediate six-bit truncation is intentionally removed for HD rendering.
Texture coordinates use floor, including negative margins.

`SnesHdFrameTransform` produces Q8 origin XY and step XY once per enhanced row.
The portable consumer and GPU upload use that same function. GPU endpoint
acceleration retains its exact double evaluation at uncertain texel boundaries.
Prepared transforms scale with output height, not the number of output pixels.
Scale 1 retains the exact native image with supersampling off.

EXTBG reuses the sampled texel, with its high bit selecting BG2 priority and
seven remaining color bits. Each background and OBJ has independent main/sub
visibility. Direct color applies only to BG1. OBJ palettes 0-3 cannot participate
in color math. Windows, color math and brightness are resolved after sampling.
The native fast renderer now delegates EXTBG/direct color to the complete PPU
reference instead of silently drawing BG1 alone.

Mosaic deliberately uses native composition to preserve authored blocks,
including EXTBG's independent horizontal mosaic. Interlace, pseudo-hires and
unsupported host overlay policies also retain native composition. The old scanline surface API and its duplicated renderer have been removed.
Its useful analytic/composition checks now exercise the frame consumer.
No compatibility shim was added.

F-Zero owns course streaming/maps, HUD layout and temporal camera selection.
Its CPU HD path now consumes the same sealed jobs as its GPU path; duplicated
spatial interpolation and sampling were removed. Temporal interpolation is
applied to raw camera values before spatial reconstruction.

## Acceptance record

Implementation and qualification are separate. Keep this issue open until
matched bsnes-hd captures, motion, representative other games, performance and
owner visual checks are complete. Supersampling is selectable independently of resolution (1=off, 2..4 per axis),
with persistence and launcher controls in the shared host and F-Zero.

Analytic tests cover exact/quantized perspective, zeros, sign changes and camera
splits. Composition checks compare centered samples against the complete native
PPU, including EXTBG/direct color. GPU checks cover non-power-of-two scales,
flips, composition, negative margins, large maps and resized targets.

Real F-Zero input `captures/bump160-deluxe-21x9/frame-001200.bin` reconstructs
all 177 track rows. Original/candidate captures and matched bsnes-hd frames have
been inspected; see the geometric comparison below.

Windows: complete PPU suite, F-Zero renderer suite and 772 GPU ROI comparisons
pass. Linux/WSL: PPU-HD composition suite, reconstruction tests (also with
NDEBUG) and the GPU-disabled portable consumer compile pass. The Deck has no
system C compiler; its temporary copied fixtures were removed.

Both Windows game hosts build after supported regeneration with the current
native analyzer. A muted headless 1800-frame route passes. A hidden OpenGL 8x
desktop run passes 1800 frames/presentations with zero missed presentations;
its unthrottled 133.45 FPS is a smoke result, not a matched performance claim.

Snapshot replay has a pre-existing failure: **both original and candidate**
lose 22 master cycles (expected 539575234, restored 539575212). RAM/SPC RAM
differences were not reported. Track separately as **beads-8wg.5.122**; do not
report this lifecycle check as passed.

Matched RTX 3080 Ti capture timing, 16:9, full target, 32 measured frames after
8 warmups; columns below are milliseconds. Host contention remains a limit.

| Scale | Original total | New total | Original GPU draw | New GPU draw |
| --- | ---: | ---: | ---: | ---: |
| 4x | 6.024 | 4.954 | 0.325 | 0.236 |
| 8x | 6.720 | 5.664 | 1.261 | 0.623 |

Total includes frame preparation/upload and a completion wait. This is one
capture, not a whole-game or cross-game performance acceptance. CPU-only 4:3
4x output improved from the first replacement's 22.70 ms to 4.80 ms through
shared color/texel reuse and the existing worker pool, but remains slower than
the approximate baseline's 2.20 ms in the final matched run. Keep that fallback
cost explicit. Raw samples, source/binary identities, jobs and capacity receipts
are retained in `_wt-mode7-rebuild-fzero/build-mode7`.

Remaining visual qualification includes Pilotwings, SMW Mode 7 bosses and
Super Metroid effects, broader natural F-Zero course play and owner acceptance.
Super Mario Kart shared capture and sampled turning comparisons are recorded
below. Supersampling and retirement of the historical scanline API are implemented.
This evidence does not qualify the remaining untested games.


## Matched emulator comparison, continuation

The existing `tools/snesref` frontend drives a separately supplied bsnes-hd
10.6 libretro core. Its binary SHA256 is
`3e05e441f35ca21baa328094f6f9bf30645ea998ebb39956b154e160a8b00ed0`;
source URL and inputs/settings are retained under `build/oracle`. The stock
core does not expose WRAM, so no memory-state equivalence is claimed.
Matched scripted F-Zero frame 1200 images identify the same scene/camera.
On 280 road-edge rows at 8x, original renderer absolute edge error against
bsnes-hd averages 10.979 high-resolution pixels (95th percentile 28, max 55).
The rebuilt renderer averages 0.275 (95th percentile 1, max 11). These are
specific silhouette comparisons, not a whole-image correctness score.

Exact rational fixtures exposed floating cancellation at texel boundaries on
non-power-of-two scales. Sampling now canonicalizes roundoff within eight ulps
of the supported transform range before flooring; CPU and GPU use the same
rule. Offsets beyond that numerical bound retain floor semantics. EXTBG-only
visibility is now covered explicitly, including when BG1 is disabled.

Supersampling has a portable integer box resolve and a GPU resolve. A native
1x output with 2x supersampling is checked against the independently averaged
2x output. GPU checks cover 2..4 samples per axis, scale 1, fractional scales,
visible/full targets, flips and composition. Default sampling adds no resolve
pass. GPU scratch allocation is budgeted and reduced with device limits.

## Final implementation checks

Super Mario Kart `d302c30a63b9110a6273e754745e6b5e58f2a64d` was regenerated
with the current native analyzer in an isolated qualification worktree. Its
existing headless host, DSP-1 LLE and verified local firmware drove 3,600 frames.
A test-only hook exercised the shared capture/portable consumer every 60 frames:
27 Mode 7 frames, 1,909 perspective-corrected rows, exact 1x native parity and
bounded 4x rendering. The existing strict logic/audio/video soak also passed.
The hook does not adopt HD presentation into SMK's custom SDL host.

This real consumer exposed a rejected horizon band. Residual validation now
uses the same interior region as fitting (two coefficient units or 2.5% for
quantized DSP tables). Every subrow still rejects sign changes, poles and
coefficients beyond the bounded extrapolation range. Separate camera bands
remain separate. Analytic horizon-artifact and singularity checks cover this.
The source patch, pinned title identity and run evidence are retained under
`build/qualification/smk`. This is a shared-consumer check, not an emulator
visual acceptance of every SMK track.

Six matched turning frames (1630..1780, step 30) retain original HD, rebuilt HD
and bsnes-hd output under `build/oracle/motion`. Their identical timer/camera
scenes were inspected, including the rotated road edge at frame 1690. This is
sampled motion evidence, not a continuous-video/flicker qualification. The
owner-facing live comparison supplies synchronized native/16x panes from one
simulation, plus paused grid, straight and turning snapshots.

Final GPU checks additionally exercise 4x supersampling with a 128 KiB target
budget: admission reduces the visible target to 68x3, CPU samples agree and
caller framebuffer/viewport state is restored. Windows PPU, renderer/video/mod
suites and Linux NDEBUG reconstruction checks pass.

A later matched capture timing under different contention produced 4x totals
3.874/4.637 ms (original/rebuild) and 8x 4.050/4.274 ms; draw times were
0.145/0.134 and 0.347/0.353 ms. Together with the earlier measurements, this
is mixed and cannot establish a performance win or reproducible regression.
The slower CPU fallback remains explicit. Retain both sets of samples; this
correctness replacement is prepared for review, not performance-qualified for
all games or promoted to a release.

Final F-Zero built checks: 16x single-player plus 2x supersampling survives four
window/aspect setups, 420 frames, with sampled CPU/GPU agreement. A two-player
course-0 run passes 1,440 frames, the same resize changes, sampled CPU/GPU parity
and zero missed presentations. A four-player course-14 run completes 1,440
frames through the existing CPU fallback; it records 420 missed presentations
and is only a functional check. Four-player GPU jobs are not implemented by the
current title adapter, and CPU supersampling there is costly. Do not describe
that run as 60 FPS or GPU-qualified. No external-network play was tested.

## Main integration and owner acceptance

On 2026-11-02 the owner confirmed that the F-Zero camera-alias flicker fix
appeared to resolve the reported corruption and explicitly authorized main
integration in both repositories and a new F-Zero Nextcloud release. The current
upstream main was merged with the tested reconstruction and F-Zero's existing
qualified dependencies. The complete PPU suite and772 GPU comparisons pass on
the combined engine. This clears the F-Zero visual handoff; the broader coverage
and pre-existing snapshot caveats above remain recorded, without claiming tests
that were not performed. Release tracking: beads-8wg.5.123.


## 2026-11-03 quality burndown candidate

The next quality pass replaces the least-squares reciprocal fit with a plane
through two interior reciprocal-depth anchors. This is a generic reconstruction
change; camera bands, pole/sign guards and portable/GPU interfaces remain.
The independent reference is the same pinned bsnes-hd10.6 core above.
At4x, no supersampling/filtering, seven aligned stock straight/corner captures
improve in every full-image comparison:

| Frame | Least-squares full RGB MAE | Anchor candidate full RGB MAE |
| --- | ---: | ---: |
|1200|2.844|0.771|
|1630|9.355|2.962|
|1660|6.275|3.168|
|1690|5.794|1.740|
|1720|4.437|0.474|
|1750|13.069|0.535|
|1780|11.693|0.559|

RGB MAE uses 0..255 channels and includes the entire frame. Separate declared
horizon (native rows47..85) and near-road (90..223) results are retained too;
no horizon is masked out of the full score. Both variants in this comparison
include the same F-Zero restoration of native additive distance haze. Native
alignment is checked with HD disabled; core WRAM remains unavailable.

The remaining distant texture difference has a concrete source-coordinate
cause. The reference truncates negative floating coordinates toward zero;
the portable/GPU production sampler floors them consistently with native
sampling. A temporary truncation-only diagnostic at frame1630 lowers VRAM-only
horizon RGB MAE from16.236 to1.658 and the fraction of channels differing by
more than8 from14.684% to0.00355%. That diagnostic was reverted; its binary and
results are retained. Full-course extension accounts for additional distant
pixels and is evaluated separately. Original5-bit color math and native artwork
remain the policy; matching a reference implementation's rounding differences
is not by itself a reason to change them.

The same pass fixes half-color math when CGWSEL clips main color to black
(`28f260c4`, beads-8wg.2.194), with independent known-color tests for native,
portable-HD and GPU paths. The seven-test PPU suite, F-Zero renderer/mod tests,
772 GPU region comparisons and non-GPU compile check pass. HD detail and Edge
smoothing now identify the two controls separately; the existing persisted IDs
and choices are retained while useful scale/default qualification remains open.

These are candidate results, not an overall quality, motion, shared-game or
release acceptance. F-Zero's twelve-area matrix remains authoritative. Raw
comparison inputs/settings, image metrics and binaries are retained in the
owner's isolated quality workspace; the F-Zero evidence index records identities.

### Validated band fallback and bounded motion acceptance

Engine `07716db3` retains interior reciprocal anchors first and tries the
previous whole-band regression only if that fit fails the same residual,
pole/sign and subrow guards. Actual SMK coefficient tables cover both opposite
quantization cases. Anchors alone reduced corrected rows664 versus historical
1909; the corrected fit reaches2241 across27 sampled frames while retaining
sampled native parity and historical final video/logic hashes/master cycles.
The pinned SMK consumer `d302c30` was regenerated with the supported native
analyzer and passed3600 strict DSP1LLE frames. This is not adoption into its
custom HD host or a cross-game performance claim.

All302 retained F-Zero4x SS1/SS2 images remain byte-identical. The owner accepts
both151-frame stock cornering clips as comparable to bsnes-hd. F-Zero HD-03's
straight/turning geometry checkpoint is resolved; its other ten areas remain
open. Seven PPU suites,772 real-GPU comparisons, F-Zero renderer tests, Ubuntu
GCC13.3 NDEBUG reconstruction and the GPU-disabled object compile pass.
Current4P8x/SS2 and fresh2P actual GPU gates pass driving, contacts, resizing
and transitions. Requested120Hz save/load retains sampled HD correctness;
state browser/rewind open-cancel checks pass in a private enabled profile.
Committed rewind and the pre-existing22-cycle snapshot replay defect are not
cleared by that check. No broad quality, sustained-rate or release claim follows.

### Exact nearest presentation

Engine `86fb6c19` shares the passthrough shader with game-owned HD presenters.
Nearest HD output uses integer framebuffer pixel centers; floating interpolated
UVs selected adjacent texels at exact boundaries. A patterned2048x1792 texture
at768x576 reproduces180,224 legacy mismatches, now0;1280x720 also matches0.
The772 existing GPU comparisons and shared Windows presenter object compile
pass. Native/linear paths keep their existing UV policy; presets are separate.

F-Zero `0faad3c` now matches44/44 actual full-raster and visible-sample output
files, while all default visible files remain byte-identical to the earlier
accepted binary. Current save/load/overlay and4P8xSS2 runtime gates pass.
Same-host2P4xSS2 A/B/B/A is37.419,37.460,37.555,36.424FPS: neutral within
variation. A later full-raster run gives35.877FPS, not an interleaved speedup
claim. Maximum target allocations are37,013,080bytes visible versus97,771,520
full. Setup/resize stalls remain, so sustained cadence is still unqualified.
Owner passes both the prepared jump/landing and interpolated camera-alias clip.
F-Zero now resolves HD-01/03/04/07; other quality/default/runtime areas remain.
