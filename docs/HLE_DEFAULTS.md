# HLE defaults and the LLE reference

The shared desktop runner defaults to `SNESRECOMP_FRAME_IMPL=HLE` on Windows
x64. Other platforms retain `LLE` until measured there. This applies to games
using `runner/runner.cmake` and the shared desktop frame service; games with
their own presentation loop are outside this qualification.

LLE remains the functioning reference for correctness investigations. HLE
keeps the same caller interface and output behavior while changing how the
work is done. Qualified, materially faster replacements should become the
normal build after focused correctness checks and owner play validation.
Unqualified experiments remain opt-in; this default does not enable them.

To opt out, add this option to the game's usual CMake configuration and rebuild:

```powershell
cmake -S . -B build-lle -DSNESRECOMP_FRAME_IMPL=LLE
cmake --build build-lle --config Release
```

Keep the game's other required configuration arguments. The selection is fixed
in the executable. LLE may use more CPU and achieve lower uncapped FPS, but is
available to check a suspected HLE correctness problem. Use
`-DSNESRECOMP_FRAME_IMPL=HLE` to explicitly select the normal Windows x64 path.
An existing cache preserves its previous selection; remove just that cache
entry or set HLE explicitly when upgrading an old LLE build. CMake reports the
selection, and the runtime's frame-service summary identifies what ran.

## Qualified implementation

HLE composes each supported frame in cached host memory, then uploads it with
one write-only row copy. This avoids expensive reads from mapped streaming
texture memory. Drawing, optional blending, frozen-menu snapshots, overlays,
frame delivery, audio, and guest execution retain their existing contract.
The maintained LLE path composes directly into the presenter buffer except
when blending already requires staging. No game-specific guest routine is
replaced by this change.

One matched uncapped Windows x64 SDL/audio pair per game produced:

| Game | LLE FPS | HLE FPS | FPS increase | Process CPU reduction |
|---|---:|---:|---:|---:|
| Mega Man X | 340.936 | 889.911 | 161.02% | 67.17% |
| Doom | 250.122 | 443.316 | 77.24% | 43.12% |
| Super Metroid | 427.737 | 1259.308 | 194.41% | 66.67% |

Each pair completed the same simulation and presentation count with normal
rendering and audio, profiling/capture disabled, and pacing removed. Routes
include boot and menus: MMX reaches intro-highway movement/jump/fire; Doom
reaches E1M1 movement/turn/fire; Super Metroid uses natural attract gameplay.
MMX terminal WRAM/VRAM/CGRAM/OAM checks matched. One basic adaptive presentation
check was clean, and the owner accepted all three HLE games at normal speed
with adaptive widescreen on 2026-10-07.

The measured binaries and exact owner responses are recorded in the
[qualification evidence](qualification/frame-service-windows-20261007.json).
Newer title changes are preserved during integration; these measurements and
playchecks identify the recorded binaries, not every subsequent title revision.
Foreign PSX compiler activity was present. These are bounded observations on
this Windows host, not statistical guarantees, whole-game coverage, or claims
about mobile/Xbox performance. Super Mario Kart and Super Mario World use
separate loops and were excluded. The unrelated 8bpp experiment remains draft
in [PR 151](https://github.com/RetroPortingToolKit/snesrecomp/pull/151).
