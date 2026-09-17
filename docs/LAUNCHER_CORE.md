# snesrecomp as a launcher-hosted core

Branch `feat/launcher-core`. Status: **design recorded, no code changed yet.**
Written 2026-09-14 from a survey of this tree at `origin/main` 4d42cab.
Line references are pinned to that commit.

## 1. Context

One launcher process (`retcomm-launcher`) becomes the single front end for
every recompiler, owning the window, configuration, library, netplay browsing
and mod selection. snesrecomp, psxrecomp and later n64lle become "cores" it
manages. Per-game repositories are retired in favour of data-only **profile
packages** plus mods.

The shared constraint, from `recomp-ai-rules/MODS.md` §8, is that a static
recompiler bakes one program image into one executable, so a variant ROM still
needs a variant binary. A core here is therefore both a **build component**
(emitters plus `runner.cmake`, invoked by the launcher to scaffold a title from
ROM + profile) and a **runtime contract** (what the built title speaks to the
launcher). The per-title binary remains; the per-title repository does not.

The transport decision is shared with psxrecomp and recorded in full in that
repo's `docs/LAUNCHER_CORE.md`: **the title runs as a child process and pushes
frames and audio to the launcher over shared memory.** Out of process, for
crash isolation and because n64lle is a Rust tree that a C++ in-process
interface would not serve.

## 2. This tree is already most of the way there

Unlike psxrecomp, snesrecomp does not need a control-flow inversion. It already
has the pieces a core contract is made of:

| Piece | Where | Note |
|---|---|---|
| Run one frame | `runner/src/cpu/common_rtl.c:652`, decl `common_rtl.h:309` | `RtlRunFrame(uint32 inputs)` — inputs in, guest advanced one frame |
| Per-title vtable | `RtlGameInfo`, `runner/src/cpu/common_cpu_infra.h:112` | initialize / run_frame / draw_ppu_frame / state save/load |
| Host descriptor | `SnesDesktopHostGame`, `runner/src/desktop/host_main.h:58-142` | already libretro-shaped: `prepare_frame`, `draw_frame`, `presentation_hz`, `before/after_run_frame` |
| Frame out | `RtlDrawPpuFrame`, `host_main.c:907` | the PPU's 256-wide ARGB output, pitch 256*4 |
| Audio out | `RtlRenderAudio(int16*, frames, channels)` | pull model, SDL only calls it from `FillAudioBuffer` |
| Per-frame hook | `FrameDumpCallback g_framedump_callback`, `runner/src/debug/framedump.c` | an existing engine-side frame callback |
| Headless | `SNESRECOMP_HEADLESS`, plus script/framedump paths | already runs without a window |
| External pacing | `SnesHostBarrierHooks`, `runner/src/netplay/snes_host_app.h` | netplay already hands frame cadence to an outside driver |

That last row matters: the netplay path already proves an external driver can
own the frame cadence, which is structurally the same thing the launcher will
do.

**The gap is one file.** `runner/src/desktop/host_main.c` (3,852 lines) fuses
three separable things: the frame stepper (already clean), SDL window,
renderer and audio-device ownership, and on the order of two thousand lines of
launcher/config/ROM-resolution/overlay/pacing policy. The cut is to keep the
stepper plus the `SnesDesktopHostGame` hooks as the core, move SDL ownership
wholesale to the launcher, and make the policy layer a launcher service the
core queries.

The awkward part is the modal overlay loops (`RunSavestateMenuLoop:1602`,
`RunRewindLoop:1651`, `PresentFrozenWithOverlay`): they re-enter SDL event
pumping and presentation from inside the core's own loop. Those become
launcher-side states.

## 3. Per-title host code: what moves upstream

The ports are in three different eras, and the picture is much better than the
oldest one suggests.

| Repo | Hand-written host C | Framework pin |
|---|---|---|
| `SuperMetroidSNESRecomp` | ~340 lines | current |
| `SuperMetroidRecomp` | ~3,700 lines | current |
| `MetalWarriorsSNESRecomp` | ~18,800 lines | ~240 commits behind |

The newest scaffold is already fully engine-hosted: `main.c` is a host
descriptor literal and a call into `snesrecomp_desktop_main`, `game_rtl.c` is
the port proper (what one frame means, NMI/IRQ at the hardware edge, HDMA per
line), and the rest is trace hooks and hand bodies. That shape is the target
for every port.

The mature ports additionally carry a host-side re-rasterizer
(`sm_renderer.c`, `mw_renderer.c`). These exist **not** because the engine
lacks a PPU. The engine rasterizes all eight modes including mode 7
(`runner/src/snes/ppu.c:1920`) and has a generic widescreen path
(`runner/src/snes/ws_shadow.c`, `PpuSetExtraSpace*`). The port renderers exist
to invent pixels outside the console's 256-column field, reconstructing them
from game-specific structures that only ROM knowledge can locate. That is an
enhancement layered on the faithful raster and byte-identical when off.

Measured against each other, roughly **500 lines per port are copied generic
code** that belongs upstream: a Mode-1 compositor (tile fetch, window logic,
colour math, brightness, the snapshot struct and its capture serialization),
plus aspect and viewport policy in `*_video.c`, plus mod-provider boilerplate
in `*_mods.c`. Four helpers are byte-identical across ports today
(`read16`, `tile_pixel`, `window_condition`, `rom_bytes`). The remaining 800 to
1,400 lines per port are irreducible ROM knowledge (`$88:8CC6`, `$80:86B6`,
`$7E:42B3`, `$8B:8AD9`) and stay with the title.

The lifecycle contract is already identical across ports and is the reusable
shape:

```
BeginFrame(ram, n) → CaptureLine(ppu, line) → EndFrame(stock) → Draw(out, viewport, hud, alpha)
```

Hoisting that contract plus the generic compositor into the engine, with the
title supplying only its ROM knowledge behind the hooks, is the single largest
de-duplication available in this tree.

## 4. Mods

Two systems coexist and they must converge on the engine one.

- **Engine-level, and the one to keep:** `runner/src/mods/mod_runtime.cpp` (3,473
  lines) with a C ABI, `.snesmod` archives, netplay mod-set reconciliation,
  framework-owned staging (`SNESRECOMP_MOD_CATALOG_DEST`) and a build-time
  catalog verifier. Gated on `SNESRECOMP_ENABLE_MODS`.
- **Legacy, per-port:** `sm_mods.c` and `mw_mods.c` hand-build a
  `RecompLauncherCModProvider` and override the framework provider through the
  host's `mods_provider` hook. They are UI shims over an `.ini`, with no
  packages, no archives and no netplay participation. Neither of those two
  repos sets `SNESRECOMP_ENABLE_MODS`, so neither links the mod runtime at all.

Under a launcher-hosted core the launcher owns mod selection, so the
per-port provider hook goes away and those ports adopt the package system.

## 5. Staging

1. **Adopt the shared core protocol** defined alongside psxrecomp.
2. **Split `host_main.c`**: stepper and host descriptor stay, SDL ownership
   leaves, policy becomes a launcher query. `--headless` plus the framedump
   callback are the base to build on.
3. **One SNES title runs in the launcher window.** The newest scaffold shape
   (`SuperMetroidSNESRecomp`) is the cheapest first subject.
4. **Hoist the generic compositor** and the lifecycle contract upstream; leave
   ROM knowledge in the title.
5. **Profile packages** replace the per-title CMake and config; the launcher
   scaffolds from ROM + profile.
6. **Retire the per-port mod providers** in favour of the package system.
7. **Move the modal overlay loops** to launcher-side states.

## 6. Open question

Whether snesrecomp or psxrecomp should reach the launcher window first.
snesrecomp is closer by a wide margin, because `RtlRunFrame` and
`SnesDesktopHostGame` already exist and psxrecomp has neither. Proving the
protocol here first would de-risk the harder tree.
