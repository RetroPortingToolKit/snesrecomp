# Legacy Mode 7 line images

`SNESRECOMP_LEGACY_MODE7_IMPL=REFERENCE|NATIVE` selects the shared image-only
service at build time. The default remains `REFERENCE`. CMake reports the
effective choice; retain that configuration with a build's identity.

Both implementations enter through `ppu_draw_whole_line_legacy`. This option
does not select a game's renderer, enable widescreen, or change the optimized
PPU's existing Mode 7 implementation. Games selecting the legacy PPU receive
the shared service; every other mode keeps its existing implementation.

`REFERENCE` retains the scalar legacy renderer in `runner/src/snes/ppu_legacy.c`.
`NATIVE` uses `runner/src/snes/ppu_mode7_line.h` to generate an entire Mode 7
image line. It walks affine coordinates incrementally, samples mosaic groups
once, shares matching BG1/BG2 samples, and resolves window and color decisions
once per span using the shared register-window job. Native sprite ranks and
EXTBG background ranks replace repeated priority-list traversal.

The replacement promises exact legacy line output, including the reference's
current behavior for signed centers/scroll, flips, overflow/character fill,
transparent texels, EXTBG, direct color, windows, main/subscreen color math,
brightness, negative margin mosaic columns, captured VRAM and extra-object
colors. It preserves the legacy pseudo-hires image behavior; this change does
not correct the reference's documented subscreen clipping/math limitations.
Output remains B,G,R,0 bytes with the supplied pitch and active side margins.

The shared caller still prepares the transform and performs live sprite
evaluation, overflow/status updates and raster/HDMA/IRQ ordering. The image job
adds no persistent state and never modifies VRAM, CGRAM, OAM, registers or
background priority buffers. It requires no save-state schema or migration,
runtime HLE switch, shadow renderer, CPU feature or platform-specific code.
Current-build save/load, rewind and rollback remain caller contracts.

Run `python tests/ppu/run_legacy_mode7.py --cc /path/to/gcc` for the focused
differential test. It compiles the maintained reference translation unit with
its public entry renamed, then links a separately compiled native translation
unit. The test compares complete output buffers and PPU state from identical
inputs. Product binaries execute only the selected implementation. Built-game qualification
must record the game, engine/configuration, actual workload and host costs;
a component test alone does not establish a runtime gain.
