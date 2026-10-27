# Your first SNESRecomp port

This guide takes you from a clean checkout to a generated, buildable game
project, then shows you where the porting work starts. You use a terminal,
an editor and your own cartridge dump. No AI tool or account is required.

The worked path is **Windows x64, Visual Studio 2022 and PowerShell**. Keep
the first attempt simple: one unmodified ROM revision, the standard launcher
and the original game presentation. Add enhancements after the game works.

If you only want to play an existing port, choose a title in the
[game list](../README.md#games), open its download page and follow that
project's ROM-selection instructions. You do not need to create a project.

## 1. Install the tools once

Install these before opening your terminal:

- [Git for Windows](https://git-scm.com/downloads/win), including Git Bash.
  The PowerShell setup command uses its Bash internally.
- [Python for Windows](https://www.python.org/downloads/windows/), version
  **3.11 or newer**, available as `python` on PATH.
- [Visual Studio 2022](https://visualstudio.microsoft.com/vs/older-downloads/)
  with **Desktop development with C++**, the Windows SDK and **C++ CMake
  tools for Windows**. Microsoft's [installation notes](https://learn.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio?view=msvc-170#installation)
  show the workload.
- [Rust through rustup](https://rust-lang.org/tools/install/), using the
  default Windows MSVC toolchain. Rust builds the analyzer; you do not need
  to write Rust to port a game. The analyzer requires Rust 1.85 or newer.

Open **Developer PowerShell for VS 2022** from the Start menu after installing
everything. Run:

```powershell
git --version
python --version
cargo --version
cmake --version
```

Each command should print a version. Fix a missing tool before continuing;
opening a fresh Developer PowerShell often resolves a newly installed PATH.
You do not need a separate SDL installation: the default build can download
and build the framework's pinned SDL3 dependency.

## 2. Choose a ROM and a workspace

Use your own unmodified `.sfc` or `.smc` cartridge dump. For a first project,
choose a game without an enhancement chip and check the
[game list](../README.md#games) for an existing port you could contribute to.
Keep the ROM outside the source repository.

The examples use `D:\SNESDev` for source, build output and temporary files,
and `D:\ROMs\My Game.sfc` for your dump. Replace those paths with folders on
your development drive. Allow several gigabytes for dependencies and builds.

```powershell
New-Item -ItemType Directory -Force D:\SNESDev\Temp | Out-Null
$env:TEMP = 'D:\SNESDev\Temp'
$env:TMP = $env:TEMP
$env:CARGO_HOME = 'D:\SNESDev\Cargo'
Set-Location D:\SNESDev
git clone --recurse-submodules https://github.com/RetroPortingToolKit/snesrecomp.git
Set-Location snesrecomp
python tools/new_project/probe_rom.py 'D:\ROMs\My Game.sfc'
```

The probe describes the cartridge and prints its identity. For this first
attempt, look for `coprocessor=none` and a valid checksum/complement check.
That check catches some bad images; it does not prove a dump is authentic.
If it reports an unexpected mapping, chip or invalid checksum, resolve the
ROM identity before generating code. The [compatibility table](../README.md#rom-compatibility)
explains the framework's cartridge support.

## 3. Create the project and generate C

From the framework checkout, run this single setup command. Replace the ROM
path; keep the other settings for your first attempt:

```powershell
powershell -File tools\new_project\setup_project.ps1 `
  -Rom 'D:\ROMs\My Game.sfc' -Dir D:\SNESDev -Name 'My First Port' -Yes `
  --players 1 --recomp-ui --no-netplay --no-fetch-boxart --no-github `
  --generate --no-build
```

This creates `D:\SNESDev\MyFirstPortSNESRecomp`. It records the ROM's
checksums, checks out the framework and launcher dependencies, generates C
and initializes a local Git repository. It does not create or publish a
GitHub repository. The first generation also builds the Rust analyzer, so
expect compiler output and dependency downloads.

Wait for generation to finish successfully. You should have generated `.c`
files under `src\gen`, a `rom_identity.txt`, and a `CMakeLists.txt`. A warning
or failure is something to investigate, not a successful setup.

## 4. Build and open the launcher

```powershell
Set-Location D:\SNESDev\MyFirstPortSNESRecomp
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
cmake --build build --config Release --parallel
.\build\Release\MyFirstPortSNESRecomp.exe
```

The first build takes longer because it builds dependencies too. Keep the
executable beside the assets and runtime libraries that CMake stages for it.
The launcher should remember the ROM used during setup; select that same
dump if it asks. Leave optional mods off and press **Play**.

**A successful build is your first milestone. A working game is the next
one.** The scaffold uses the shared beam-aligned frame driver to execute
the original program and deliver hardware interrupts. Some games get far
with it; others need game-specific integration before they boot or play
correctly. A launcher, a title screen and a complete campaign are different
levels of progress.

## 5. Make the first game behavior reliable

Start with one small, repeatable goal: cold boot, reach the title menu,
start a game and exercise the first playable scene. Check input, scrolling,
sprites and audio against the original game in a trusted emulator. Keep a
short record of what passed, what failed and what you have not tested.

Open these files in your editor:

- `src/game_rtl.c` connects the title to the frame driver. This is where
  game-specific scheduling and integration belong.
- `recomp/symbols.toml` names functions and records analysis roots;
  `recomp/bank*.cfg` supplies analysis directives. Change those inputs when
  a function boundary or an indirect call needs explaining.
- `src/main.c` names the application and connects it to the shared desktop
  host. The framework supplies the launcher, display, input and save UI.

When you change analysis configuration, regenerate with Git for Windows'
Bash and rebuild. This uses its default installation path:

```powershell
& 'C:\Program Files\Git\bin\bash.exe' tools/regen.sh --rom 'D:/ROMs/My Game.sfc'
cmake --build build --config Release --parallel
```

Do not edit generated files in `src/gen`: regeneration replaces them. Code
that has not been safely translated can use the interpreter tier while you
investigate. A first port does not need every routine compiled before it
can be tested.

If the program crashes or diverges, record the last working scene, the
inputs that lead to the problem and the relevant log. Investigate that
specific failure before adding more features. The [frame-driver notes](FRAME_MODEL_HOSTS.md)
explain scheduling; [reference debugging](../README.md#reference-debugging)
explains how to compare execution with an independent emulator.

## 6. Keep the stock game working as you add features

Once the original game is reliable, add one enhancement at a time and replay
the same checks. A wider picture needs correct scenery, object visibility
and HUD placement. A multiplayer menu needs an actual synchronized game.
Neither comes automatically from a checkbox in the scaffolder.

A **mod** adds behavior or presentation to a supported game. A **content
variant** selects another explicitly integrated program or content set,
often with its own save namespace. Arbitrary ROM hacks and regional revisions
do not become supported simply by selecting another file. For this first
port, keep the one stock revision; implement new mod behavior against it
using the [mod-authoring guide](MOD_AUTHORING.md). The
[variant reference](CONTENT_VARIANTS.md) is for titles that need that later
integration.

Commit your hand-written source, configuration and documentation. Keep ROMs,
generated code, extracted assets, saves and local build output out of Git.
When you are ready to share a build, use the project's packaging workflow
and include the [third-party notices](../THIRD_PARTY_ATTRIBUTION.md).

For the next development steps, use the [project setup reference](GAME_PROJECT_SETUP.md).
It covers the scaffold's layout, regeneration and release tooling. The
[scaffolder reference](../tools/new_project/README.md) documents advanced flags
and other host platforms.
