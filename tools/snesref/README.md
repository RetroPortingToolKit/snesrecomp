# snesref

A standalone, hardware-accurate **reference interpreter with debugging tools**,
used as the differential oracle when chasing bugs in the recompiled build.

> Formerly named `mmxref`, then `snes-oracle`; settled on **snesref** to pair
> with the Genesis equivalent [`mdref`](https://github.com/mstan/mdref).

Under the hood it is a minimal SDL2 [libretro](https://www.libretro.com/)
frontend that loads a real SNES emulator core (an interpreter such as
`snes9x_libretro.dll`) and drives it while capturing the same instrumentation
the recomp runner's `debug_server` exposes. You play the game on a known-good
interpreter, capture a trace, and diff it against the recompiled run to pinpoint
the first divergence — register, RAM byte, or frame.

It is the successor to the old in-process `runner/snes9x-core` oracle (now
removed): instead of statically linking a patched emulator into every game's
`Oracle|x64` build, you run this one small tool against any libretro SNES core.

> Stood up while debugging Mega Man X (the Rangda Bangda eye and the Spark
> Mandrill dash-jump turtle), so state filenames retain an `mmx_*` prefix — but
> it works with any SNES ROM and any libretro SNES core.

## Why a separate interpreter, not the recompiler

The recompiler translates 65816 to native C ahead of time; subtle timing/state
bugs only show up as a *divergence from real hardware*. You need a trusted,
cycle-faithful reference to diff against. An interpreting emulator core is that
reference. Keeping it as a separate tool (rather than embedded in the runner)
means the shipping game exes carry none of it, and the reference can be swapped
for any libretro SNES core.

## Features

- **Per-frame WRAM diff trace** → `snesref_trace.jsonl`, one record per frame of
  changed WRAM bytes, in the exact JSON shape as the recomp `debug_server`'s
  `wram_writes_at`. Drop it beside a recomp capture and diff frame-by-frame.
- **Save/load state slots** (`Shift+F1` through `Shift+F9` to save, `F1`
  through `F9` to load) so you can park at a hard-to-reach game state and
  re-run the suspicious window deterministically.
- **Recomp-matched input** — the keyboard map mirrors the recomp runner's
  keybinds, so the same inputs reproduce the same run on both sides.
- **Any core, any ROM** — the emulator core is `argv[1]`; nothing is hard-wired
  to a specific core or game.
- **Fresh-capture toggle** (`F5`) to clear the trace and start a clean window.
- **Tiny + dependency-light** — one C++ translation unit, SDL2, and the libretro API
  header. No build coupling to the recompiler.

## Build

### CMake (macOS, Linux, or Windows)

Install the SDL2 development package, then configure and build from the
SNESRecomp repository root:

```sh
cmake -S tools/snesref -B build/snesref -DCMAKE_BUILD_TYPE=Release
cmake --build build/snesref
```

On macOS, Homebrew's `sdl2` formula supplies the required headers, library, and
CMake package. The resulting executable is `build/snesref/snesref`.

### Existing Visual Studio batch build

```bat
:: 1. extract the SDL2 VC dev package here as SDL2-2.30.9\   (libsdl.org)
::    or set SDL2_DIR to an extracted package elsewhere
:: 2. build (OUT_DIR optional)
build.bat
```

Produces `snesref.exe` (in `OUT_DIR` if set).

Linux / macOS (SDL2 development headers from the system package manager):

```sh
./build.sh
```

Produces `snesref`. The core is loaded with `dlopen` there and
`LoadLibrary` on Windows, so the same source builds on both.

## Run

```text
snesref <libretro-core> <rom.sfc>

Windows example:
snesref.exe snes9x_libretro.dll game.sfc

macOS example:
./build/snesref/snesref snes9x_libretro.dylib game.sfc
```

Pass a path to a separately supplied libretro core: a DLL on Windows, a dylib
on macOS, or a shared object on Linux. Windows batch-build users should also
place `SDL2.dll` beside `snesref.exe`.

```sh
./snesref <core.so> <rom.sfc>
# e.g. ./snesref ~/.config/retroarch/cores/snes9x_libretro.so mmx.sfc
```

Headless capture requires neither a window nor dummy SDL video drivers:

```sh
SNESREF_HEADLESS=1 SNESREF_FRAMES=2000 \
  SNESREF_FRAME_DUMP_DIR=frames ./snesref core.so game.sfc
```

### Deterministic capture

Environment variables select non-interactive capture outputs:

| Variable | Purpose |
|---|---|
| `SNESREF_FAST=1` | Hide the window and disable frame pacing. |
| `SNESREF_HEADLESS=1` | Run without SDL video or live keyboard/controller input; scripted input and capture outputs remain available. Headless runs are unpaced. |
| `SNESREF_FRAMES=N` | Exit after exactly `N` emulated frames. |
| `SNESREF_INPUT_FILE=path` | Apply deterministic scripted joypad input. |
| `SNESREF_WRAM_FILL=byte` | Fill exposed system WRAM before frame 1; use `0` to match SNESRecomp's hard-reset state. |
| `SNESREF_WRAM_DUMP=path` | Write the core's final exposed system WRAM image. |
| `SNESREF_TRACE_FILE=path` | Write low-WRAM change records as JSONL. |
| `SNESREF_WAV=path` | Write core output as a PCM WAV file. |
| `SNESREF_FRAME_DUMP_DIR=path` | Write selected frames as 256x224 BGRX raw files. |
| `SNESREF_FRAME_DUMP_FROM=N` | First frame eligible for a frame dump. |
| `SNESREF_FRAME_DUMP_TO=N` | Last frame eligible for a frame dump. |
| `SNESREF_FRAME_DUMP_STEP=N` | Dump every `N`th eligible frame. |
| `SNESREF_APURAM_TRACE_FILE=path` | Trace SPC RAM when supported by a patched core. |
| `SNESREF_DSPREG_TRACE_FILE=path` | Trace S-DSP registers when supported by a patched core. |
| `SNESREF_SCRIPT=path` | Scene-keyed script (grammar below). Exit 0 on `quit`, 3 on an `until` timeout, 6 on a malformed script. Headless runs without `quit`/`SNESREF_FRAMES` exit at end of script. |
| `SNESREF_DUMP_DIR=path` | Directory for script `dump` outputs (default: cwd). |
| `SNESREF_SRAM_IN=path` | Load into `RETRO_MEMORY_SAVE_RAM` after `retro_load_game`, before frame 1. |
| `SNESREF_LAYERS=hex` | Layer mask, bit0=BG1 bit1=BG2 bit2=BG3 bit3=BG4 bit4=OBJ (default `1F`). Maps to snes9x `snes9x_layer_1..5` (5 = sprites). |
| `SNESREF_NO_COLORMATH=1` | `snes9x_gfx_transp=disabled`. |
| `SNESREF_NO_CLIP=1` | `snes9x_gfx_clip=disabled` (windows). |
| `SNESREF_CORE_OPTIONS=k=v;k=v` | Override any core option. |

Core options: every option the core declares (`SET_VARIABLES` /
`SET_CORE_OPTIONS*`) is answered with its declared default, as RetroArch does.
Leaving them unanswered is not equivalent -- snes9x then leaves
`SuperFXClockMultiplier` at 0 and every GSU game (Yoshi's Island, Star Fox)
freezes. Non-default answers are logged as `core option k=v (default d)`.

### Script (`SNESREF_SCRIPT`)

One command per line, `#` comments. Commands are evaluated at frame
boundaries (before each `retro_run`). The grammar and the per-frame semantics
are shared with the recomp host (`runner/src/desktop/host_main.c`), so one
file drives both sides.

```text
wait N                                  # N idle frames (consecutive waits accumulate)
press <btn[+btn...]> [N]                # hold for N frames (default 1); + , | all join
poke <addr> <hexbytes>                  # write WRAM bytes on one frame
pokefor <addr> <hexbytes> N             # write WRAM bytes on N frames
forcepoke <addr> <hexbytes>             # write WRAM bytes every frame from the next one on
loadstate N | reset                     # loadstate: ignored (warning); reset: retro_reset
until <addr> <op> <hexval> [timeout]    # op == or !=, 8-bit; timeout frames (default 36000)
until16 <addr> <op> <hexval> [timeout]  # 16-bit little-endian
dump <tag>                              # write state of the frame just completed
quit                                    # exit 0
```

- `addr` is a WRAM offset in hex, `0..1FFFF` (`$7E:0118` -> `0118`,
  `$7F:0000` -> `10000`). Buttons: `b y select start up down left right a x l r`.
- Hold-type commands (`press`, `poke`, `pokefor`, `forcepoke`, `loadstate`,
  `reset`) run for their hold frames and are then followed by **one idle
  frame**. `press a 2` then `press b` = frames A, A, idle, B, idle.
- `until` true, `dump`, `quit` cost zero frames and have no trailing idle
  frame. A false `until` costs one idle frame per re-check.
- Frame numbers are 1-based `retro_run` counts; each executed command logs
  `script f=<frames completed> ...` to stdout.

### Dump outputs (`dump <tag>`, into `SNESREF_DUMP_DIR`)

| File | Content |
|---|---|
| `<tag>.fb.bmp` / `<tag>.fb.bgrx` | Last video frame, 24-bit BMP and raw WxH BGRX (W,H as the core reported; 512 wide is written as-is). |
| `<tag>.wram.bin` / `.vram.bin` / `.sram.bin` | `RETRO_MEMORY_SYSTEM_RAM` (128 KiB), `VIDEO_RAM` (64 KiB), `SAVE_RAM` (for snes9x SuperFX carts this is the GSU Game Pak RAM, `$70:0000`...). |
| `<tag>.cgram.bin` | 512 bytes, CGRAM as little-endian words *(patched core)*. |
| `<tag>.oam.bin` | 544 bytes *(patched core)*. |
| `<tag>.regs.json` | Last-written `$2100-$2133`/`$4200-$420D`, decoded PPU internals (BG scroll/bases, mode, TM/TS/TMW/TSW, color math, fixed color, windows, Mode 7, OBSEL/OAM, VRAM port), DMA/HDMA channels at dump time and at this frame's HDMA init *(patched core)*. |
| `<tag>.ppuw.tsv` | `frame vcounter hcounter addr value source` for every write to `$2100-$2133` and `$420C` during the dumped frame; `hcounter` in dots (0-339), `source` = `cpu`/`dma`/`hdma` *(patched core)*. |
| `<tag>.info.json` | Frame, fb size/format, core name/version, layer mask, memory sizes. |

A dumped "frame" is one `retro_run`: for snes9x it spans V=225 (vblank start)
of the previous frame through V=224, so its vblank writes come first.

### Patched snes9x core (debug exports)

`cgram`/`oam`/`regs`/`ppuw` need a snes9x libretro core carrying
`libretro/snesref_debug.{h,cpp}` (local branch `snesref-debug-exports`
of the snes9x checkout). It exports `snesref_dbg_*` (resolved with
`GetProcAddress`/`dlsym`; a stock core still works and those files are skipped
with a warning) and keeps an **always-on** ring of the last 1M PPU register
writes, hooked in `S9xSetPPU`, `S9xSetCPU($420C)` and the DMA fast paths;
the dump queries it backward, nothing is armed. Windows build (mingw, run from
PowerShell with `C:\msys64\mingw64\bin` on `PATH`):

```powershell
mingw32-make -C libretro platform=win CC=gcc CXX=g++ -j16
```

An input script contains one `start-frame:duration:hex-mask` event per line.
Events may overlap and comments begin with `#`.

```text
# Press Start for two frames, then B for one frame.
650:2:008
780:1:001
```

The 12-bit mask is:

| Bit | Mask | Button |
|---:|---:|---|
| 0 | `001` | B |
| 1 | `002` | Y |
| 2 | `004` | Select |
| 3 | `008` | Start |
| 4 | `010` | Up |
| 5 | `020` | Down |
| 6 | `040` | Left |
| 7 | `080` | Right |
| 8 | `100` | A |
| 9 | `200` | X |
| 10 | `400` | L |
| 11 | `800` | R |

POSIX shell example:

```sh
SNESREF_HEADLESS=1 \
SNESREF_FRAMES=1800 \
SNESREF_WRAM_FILL=0 \
SNESREF_INPUT_FILE=menu-input.txt \
SNESREF_TRACE_FILE=wram.jsonl \
SNESREF_WAV=reference.wav \
SNESREF_FRAME_DUMP_DIR=frames \
./build/snesref/snesref /path/to/bsnes_libretro.dylib /path/to/game.sfc
```

PowerShell example:

```powershell
$env:SNESREF_FAST = "1"
$env:SNESREF_FRAMES = "1800"
$env:SNESREF_WRAM_FILL = "0"
$env:SNESREF_INPUT_FILE = "menu-input.txt"
$env:SNESREF_TRACE_FILE = "wram.jsonl"
$env:SNESREF_WAV = "reference.wav"
$env:SNESREF_FRAME_DUMP_DIR = "frames"
.\snesref.exe C:\cores\bsnes_libretro.dll C:\roms\game.sfc
```

### Keys (match the recomp keybinds)

| Key | SNES | | Key | Action |
|-----|------|-|-----|--------|
| Arrows | D-pad | | Shift+F1-F9 | save state slot |
| Z | B (jump) | | F1-F9 | load state slot |
| X | A | | Backspace | clear the WRAM trace |
| A | Y (fire) | | Enter | Start |
| S | X | | RShift | Select |
| C / V | L / R | | Esc | quit |

## Licensing

This directory tracks only `frontend.cpp`, build documentation, and
`libretro.h` (**MIT**, RetroArch team). SNESRecomp's original code is licensed
under the repository's PolyForm Noncommercial License 1.0.0.

SDL2 is a separately supplied build/runtime dependency under the zlib license.
The emulator core is another separately supplied runtime DLL. bsnes is GPLv3;
Snes9x uses a non-commercial license. No SDL binary, emulator core source or
binary, ROM, or firmware is committed or included in SNESRecomp release
packages. See `.gitignore` and the repository's
`THIRD_PARTY_ATTRIBUTION.md`.
