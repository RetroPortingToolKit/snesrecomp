# Captured raster registers

Pure raster jobs need register values, not a complete mutable PPU. Include
`snes/raster_register_jobs.h` or `snes/raster_register_plan.h` and use
`SnesRenderRegisters` for captured register-only inputs. VRAM, palette and OAM
remain separate immutable inputs. Scene reconstruction and camera policy remain
with the consumer.

`SnesRenderRegistersLoad(source)` copies exactly 64 readable bytes into a real
typed object. The source may be unaligned. It may point to a captured register
record or the register prefix of a live `Ppu`. The caller must provide the
complete record. Do not cast a short record to `Ppu *`, or pass the compact
object to device functions that need VRAM, latches, buffers or mutable state.

```c
#include "snes/raster_register_plan.h"

SnesRenderRegisters registers = SnesRenderRegistersLoad(captured_registers);
SnesRenderWindowPlan windows = SnesRenderRegistersWindows(&registers,
    registers.window1left, registers.window1right,
    registers.window2left, registers.window2right);
SnesRenderComposePlan plan;
bool valid = SnesRenderRegistersPlanCompose(&plan, &registers, &windows,
    first_x, width, NULL, 0);
```

The compact Mode-1 tile job is `SnesRenderRegistersMode1TileRow`. Window mask,
OAM binding/index and brightness jobs retain their established names and are
available from the compact job header. These operations allocate no persistent
state and introduce no backend selector. Consumers still own their caches.

## Device and reference compatibility

`ppu_register_fields.inc` declares the register fields once for both types.
`ppu.h` checks every field offset, the following device-state offset and the
serialized prefix size in C and C++. The complete PPU, capture byte layout and
device/reference implementation retain their existing contracts.

Existing includes `raster_jobs.h` and `raster_plan.h` keep their `Ppu` entry
points. These adapters copy the captured register prefix and call the same
pure jobs. Existing device consumers need no migration. A register-only
consumer can migrate mechanically without changing render algorithms, features
or product defaults.

## Automated qualification, 2026-10-28

Issue `beads-8wg.2.167`, campaign `beads-2wy8.1`, tracks shared R9 work. MMX and
Super Metroid provide real consumers; unchanged SMW is the device-path control.
Windows/Linux shared planner and standalone contract suites, native PPU oracle,
C++17 headers and staged title renderer fixtures pass. A 32-bit syntax/layout
compile passes; this is not a linked 32-bit game or original Xbox qualification.

Matched Windows x64 GCC O3 production Metroid renderer compilation gives:

| Function | Previous stack frame, bytes | Compact registers, bytes |
| --- | ---: | ---: |
| `SmRendererEndFrame` | 412,352 | 1,456 |
| `SmRendererDraw` | 214,432 | 13,040 |
| `eye_beam_windows` | 207,776 | 2,272 |
| `power_bomb_windows` | 206,032 | 512 |

The renderer object's BSS decreases by 205,440 bytes and text by 640 bytes.
Compiler stack frames are not process peak memory measurements. The combined
candidate also includes [bounded scene-export scratch](SCENE_EXPORT_MEMORY.md).
Matched built-game qualification against engine main `a1c478d1` uses candidate
`8323d781`: MMX consumer `4fe87d3`, Metroid consumer `64ca06d`, and unchanged
SMW `324dfc8`.

All four ROM-backed MMX Magic Carpet cases, current/fresh restore and rollback,
captured images, Metroid live effects/interpolation/current/fresh restore,
production scene export and run-ahead pass. SMW's natural route has matching
gameplay pixels/WRAM/VRAM/CGRAM/PPU; its current-build fresh-process load also
passes without pausing. The unchanged, previously qualified boss-spawn logic
uses its existing 180-case evidence rather than repeating that unrelated sweep.

Five balanced pairs per stock/wide profile run through the shared capacity
pool. Every profile stays on one Windows host/configuration. All six profiles
have small mixed CPU deltas and are **neutral/inconclusive**, not demonstrated
CPU wins. Candidate image BSS decreases by 1,572,864 bytes in MMX and SMW and
1,778,304 bytes in Metroid. Per-profile median paired peak-commit reductions
are about 1.51-2.04 MiB; resident working set is a separate metric.

Combined owner gameplay/interactive rewind validation and merge remain. The
MMX/Metroid elective optimization switches remain OFF by default. These are
Windows x64 results, not original Xbox or linked 32-bit game qualification.

Commands, source hashes, raw stack reports, sections and fixture results are
retained under
`G:/Temp/Codex/snesrecomp/shared-render-registers-20261028/evidence/` and indexed
in the central campaign. See [the burndown](OPTIMIZATION_BURNDOWN.md) for the
shared-first scope and per-game acceptance rules.
