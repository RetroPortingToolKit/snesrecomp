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
Constant and zero coefficients remain constant. Fits must explain all original
rows within quantization tolerance and remain finite throughout every subrow.
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
Scale 1 retains the exact native image.

EXTBG reuses the sampled texel, with its high bit selecting BG2 priority and
seven remaining color bits. Each background and OBJ has independent main/sub
visibility. Direct color applies only to BG1. OBJ palettes 0-3 cannot participate
in color math. Windows, color math and brightness are resolved after sampling.
The native fast renderer now delegates EXTBG/direct color to the complete PPU
reference instead of silently drawing BG1 alone.

Mosaic deliberately uses native composition to preserve authored blocks,
including EXTBG's independent horizontal mosaic. Interlace, pseudo-hires and
unsupported host overlay policies also retain native composition. The historical
scanline surface API remains a diagnostic reference; desktop capture and
F-Zero use the reconstructed frame path. No compatibility shim was added.

F-Zero owns course streaming/maps, HUD layout and temporal camera selection.
Its CPU HD path now consumes the same sealed jobs as its GPU path; duplicated
spatial interpolation and sampling were removed. Temporal interpolation is
applied to raw camera values before spatial reconstruction.

## Acceptance record

Implementation and qualification are separate. Keep this issue open until
matched bsnes-hd captures, motion, representative other games, performance and
owner visual checks are complete. Supersampling is currently off; a separately
selectable supersampling control has not been implemented.

Analytic tests cover exact/quantized perspective, zeros, sign changes and camera
splits. Composition checks compare centered samples against the complete native
PPU, including EXTBG/direct color. GPU checks cover non-power-of-two scales,
flips, composition, negative margins, large maps and resized targets.

Real F-Zero input `captures/bump160-deluxe-21x9/frame-001200.bin` reconstructs
all 177 track rows. Original and candidate 4x images have been inspected; this
is a matched local comparison, not yet a matched bsnes-hd comparison.

Windows: complete PPU suite, F-Zero renderer suite and 736 GPU ROI comparisons
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

Remaining qualification: matched bsnes-hd stills and motion; Super Mario Kart,
Pilotwings, SMW Mode 7 bosses and Super Metroid effects; broader F-Zero course,
split-screen and resize gameplay; owner visual acceptance. A selectable
supersampling control and removal/migration of the historical diagnostic
scanline API are also unfinished. Do not merge or release as fully qualified.
