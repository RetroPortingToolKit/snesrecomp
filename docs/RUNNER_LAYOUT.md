# runner/src layout

The runner is organised by **layer**: the emulated machine in `snes/`, the
recompiled-code execution core in `cpu/`, and each surrounding concern in its
own folder. Before this, 76 files sat loose at the top of `runner/src/`
alongside the four folders that already existed, so there was no way to see at
a glance which code is the Super Nintendo, which is our execution scaffolding,
and which is host-side presentation that a launcher could own.

This mirrors the structure psxrecomp now uses (`psxrecomp/docs/RUNTIME_LAYOUT.md`),
deliberately, so the two engines can be read side by side and audited for what
belongs in a shared library. See [`LAUNCHER_CORE.md`](LAUNCHER_CORE.md) for the
core boundary that this layout is meant to expose.

## Layers

| Folder | Holds | psxrecomp counterpart |
|---|---|---|
| `snes/` | The machine: CPU core, PPU, APU/SPC/DSP, DMA, cart and mappers, coprocessors (SA-1, SuperFX, CX4, DSP-1, S-DD1), MSU-1, widescreen shadow | `cpu/` `gpu/` `spu/` `dma/` … (psxrecomp splits these per block) |
| `cpu/` | The recompiled-code execution layer: RTL entry points, CPU state, execution mode, dispatch shims | `cpu/` |
| `desktop/` | Host-owned surfaces: SDL window and audio, OpenGL, host clock, keybinds, OSD, overlay draw, savestate menu, launcher picker, widescreen present. **Everything here is a launcher-core candidate.** | `host/` |
| `netplay/` | Netplay, rollback, session and barrier hooks | `net/` |
| `lobby/` | Lobby client and its vendored WebSocket helpers | `net/lobby_ws/` |
| `mods/` | Mod runtime, mod audio, guarded patching, text translation, host mesh | `mods/` |
| `debug/` | Debug server, CPU/audio/PPU-DMA traces, cosim, host report, benchmark, framedump, Lua bridge | `debug/` |
| `state/` | Rewind and run-ahead | `state/` |
| `util/` | Leaf helpers with no engine dependencies: CRC32, SHA-256, base types, ROM image verification | `util/` |

`snes/`, `desktop/`, `netplay/` and `lobby/` kept their names. They are
referenced with path-qualified includes (`"snes/ppu.h"`,
`"desktop/sdl_compat.h"`, `"netplay/snes_netplay.h"`) from the engine, the
tests and the per-title ports, so renaming them would break all three for no
gain. `desktop/` is this tree's `host/`.

## Rules for new files

1. **Hardware goes in `snes/`.** The machine is one unit here; psxrecomp splits
   its blocks into separate folders because its runtime is four times the size.
2. **`desktop/` only grows things a launcher could own.** If a file there turns
   out to be load-bearing for the simulation, it is in the wrong folder.
3. **`util/` is for leaves.** Anything in `util/` that starts including engine
   headers has stopped being a util.
4. **Component folders are on the include path**, so a flat
   `#include "common_rtl.h"` still resolves from anywhere. Prefer the qualified
   form (`"cpu/common_rtl.h"`) in new code.

## Notes on the move itself

- Every file moved with `git mv`, so history follows.
- **No include line was rewritten for the sake of the move**, with one
  exception below. The engine and the ports both use flat includes for what
  used to be top-level headers (`common_rtl.h`, `cpu_state.h`, `types.h`,
  `host_report.h`, `keybinds.h`, `framedump.h`, `debug_server.h`,
  `execution_mode.h`, `audio_trace.h`, `cpu_trace.h`, `launcher.h`), so the new
  component folders were added to `SNESRECOMP_RUNNER_INCLUDE_DIRS` and to the
  test harness include roots instead.
- The exception: 17 parent-relative includes (`#include "../types.h"` and
  friends, mostly from `snes/`) had to be repointed at the new folders, because
  a relative path cannot be fixed by an include root.

### The move shipped broken, and the ports are how we found out

An earlier draft of this document claimed the three per-title ports kept
building unchanged. **They did not.** The rename commit (`0a947f4`) was 76
`git mv`s with zero content change and **updated no build file at all** —
`runner.cmake` still named every source at the top of `src/`. Nothing upstream
objected, because the recompiler suite is pure Python and the C harnesses that
would have caught it were not part of it, so the breakage was delivered to the
games. CMake reports `Cannot find source file` one file per target, so a
thirty-entry-stale list reads as three unrelated bugs *in the game's repo*:
three ports hit it separately before anyone looked at the framework.

The repair was two commits, and both are the reason this section exists rather
than a footnote:

- `9448e06` rewrote the ~150 stale `runner/src/...` references across build
  files, tests, tools and docs — the work the rename should have carried.
- `c985096` made the next rename fail *here*: `runner.cmake` now validates its
  own `SNESRECOMP_RUNNER_SOURCES` and `SNESRECOMP_RUNNER_INCLUDE_DIRS` at
  configure time and fails with one message naming every stale path, and
  `tools/check_runner_paths.py` audits (`--repo <game>`) or repairs (`--fix`)
  every reference to a runner source in a repository. `tests/v2/test_runner_paths.py`
  runs that audit in CI, so the suite cannot pass on a tree whose build files
  disagree with the layout on disk.

**If you move a file between layer folders, run `tools/check_runner_paths.py
--fix` in the same commit.** See `docs/GAME_PROJECT_SETUP.md` for the game-side
half of the same rule.

## That list of flat includes is the de-facto public API

The headers the ports include by flat name are, in practice, snesrecomp's
public surface. They are now spread across `cpu/`, `debug/`, `desktop/` and
`util/`, which is the first time that surface has been visible as a set. If
this engine ever grows a real `runner/include/` — the shape psxrecomp already
has — that list is what belongs in it.

## Reproducing the verification

```sh
bash tests/run_c_tests.sh
```

All 26 harnesses pass (verified 2026-09-17, after the `9448e06` /
`c985096` repair). Its `RUNNER_INC` list is a hand-maintained copy of
`SNESRECOMP_RUNNER_INCLUDE_DIRS` — if you add a layer folder, add it in both
places or the tests build against a header set the game build does not have.

When this layout first landed, two of the 26 (`lobby mod plan`, `account
secret path`) failed for an unrelated reason: `lib/recomp-net` was checked out
older than the commit this tree records, missing `recomp_net/chat_report.h`
and `rnet_account_set_secret_path`. That submodule is current again, so both
pass; if they start failing that way again, check the submodule before the
layout.

The layout itself is additionally checked by `python3
tools/check_runner_paths.py` (and in CI by `tests/v2/test_runner_paths.py`),
which is cheaper than a full build and catches exactly the failure this
reorganisation shipped.
