# Optional data-pack catalog

`snesrecomp_target_data_packs(target)` attaches a C ABI catalog backed by
LibArchive and RapidJSON. It is explicitly opt-in. Merely including
`runner.cmake` adds no pack discovery, dependency, UI, directory or guest
behavior to an existing game.

This extracts the transport/validation pattern used by F-Zero Forever into a
game-independent layer. Both F-Zero Forever (`f-zero-forever`) and SMW's
`feat/shared-data-packs` consume `feat/shared-content-packs`, based on main.
Their submodule pins identify the exact shared revision.

## Boundary

| Shared framework | Game adapter |
| --- | --- |
| Folder/ZIP discovery at launch | Decode FZEdit courses or SMW semantic DAT |
| Stable IDs, deterministic catalog, duplicate rejection | Group courses into cups or worlds |
| Game/base-ROM/payload-format checks | Preserve original music, physics and records |
| Payload hashes, declared capability checks | Implement and qualify each mechanic |
| Bounded, relative asset reads within the owning pack | Apply data at safe game lifecycle boundaries |
| Error callback for unavailable packs | Native title/file-selection UX, save namespace |

The framework never executes pack code, applies guest writes, switches ROMs or
imports arbitrary IPS/BPS hacks. It also does not prescribe a universal title
screen or make a new mod checkbox. Required capabilities are a game-supplied
allowlist, not a way to load ASM. Content-only SMW packs continue using its one
stock program. This branch does not introduce a program/ROM-switch registry.

## Manifest

A folder, ZIP root, or a single enclosing ZIP folder contains `pack.json`:

```json
{
  "format": "snesrecomp.data-pack",
  "version": 1,
  "game": "super-mario-world",
  "id": "thnan",
  "title": "This Hack Needs A Name",
  "base_rom_sha256": "<64 lowercase hex characters>",
  "requires": ["smw.semantic-dat"],
  "payload": {
    "format": "smw.semantic-dat",
    "file": "world.dat",
    "sha256": "<64 lowercase hex characters>"
  }
}
```

The game chooses the install directory, accepted format, base hash and available
capabilities. `snes_data_packs_scan` returns validated payload bytes. The game
must still decode and validate their semantics before adding a selectable entry.
Unknown capabilities, incompatible games/ROMs, bad payload hashes and malformed
packs are reported through a callback without disabling valid siblings.
Duplicate valid IDs reject all copies; filenames never choose a winner.
Stable IDs survive a folder/ZIP rename and should be used for save identities.

Scanning snapshots bounded payload bytes in memory and performs no extraction.
Small assets use `snes_data_pack_read`. File-based decoders and streaming audio
may opt into `snes_data_pack_directory`: folders return their existing root;
ZIPs stream into a content-addressed cache under the caller's chosen directory.
Only a complete extraction receives a completion marker outside the archive tree.
The cached payload is compared to the admitted bytes before use. The cache never
participates in discovery, so removing a ZIP removes the pack next launch.

Treat this cache as disposable, private runtime data; do not edit it or share it
between concurrent writers. Change the source folder/ZIP between launches.
Assets must remain available for the session. Install exactly one folder or ZIP
for each ID. There is no hot reload.

Limits: 128 candidates, 128 MiB per payload, 512 MiB admitted payloads, 64 KiB
manifest, 32 JSON nesting levels, 20,000 ZIP entries, 2 GiB per ZIP entry and
8 GiB total declared ZIP data. Relative asset reads have a caller-specified
allocation bound. Absolute/traversing paths, external folder symlinks, archive
links, encrypted entries, duplicate paths (case-insensitive), duplicate JSON
keys and Windows device filenames are rejected. LibArchive normalizes legacy
ZIP backslashes before path validation. Optional materialization repeats the
archive checks and contains writes within the cache.

Hashing detects corrupt or mismatched content; it is not publisher authentication.
The game-owned decoder remains the authority on supported content.

## Building and testing

Include `runner/data_packs.cmake` directly for tools without the full runner.
Linking requires LibArchive and RapidJSON headers. On this Windows toolchain:

```powershell
cmake -S tests/data_pack -B build-data-packs -G Ninja -DCMAKE_PREFIX_PATH=C:/msys64/mingw64
cmake --build build-data-packs
ctest --test-dir build-data-packs --output-on-failure
```

`tools/data_pack.py` provides a small shared writer; a game importer supplies
qualified bytes and metadata. `write_pack` refuses to overwrite an existing pack. `manifest_bytes` also
lets multi-asset importers create the envelope around their semantic index.
It does not convert ROMs or qualify mechanics.

## Consumers and validation

F-Zero wraps its `courses.json` in this envelope (`f-zero`,
`fzero.course-index`, capability `fzero-course-v1`). Shared materialization
supplies paths to the existing FZEdit decoder, title resources and PCM streaming.
Course filename to `music/<stem>.pcm` routing stays in F-Zero; no whole-song
allocations or game-specific audio rules are introduced in the catalog.
`data_pack_io.hpp` exposes the same bounded file, JSON and contained-path helpers
for game-owned semantic adapters.

SMW wraps the already qualified 30-stage THNAN semantic DAT
(`super-mario-world`, `smw.semantic-dat`). It consumes bytes directly and keeps
its existing title selection, stock program, stage hooks and save namespaces.
Full-hack qualification remains separate from this transport refactor.

This branch also carries dependencies needed by these consumers: the existing
collection catalog/guarded IPS-BPS importer (`content_pack.h`, a lower-level
import tool, not the shared JSON envelope), host MSU path resolution, scoped
keyboard defaults, pre-opcode redirects and finite PPU captures. A status-returning
`RtlTryWriteSram` lets content selection stop if the old namespace cannot be saved;
the existing void `RtlWriteSram` keeps its original direct-write behavior. Configured mod
resource paths have a bounded C accessor.

Windows validation on 2026-09-25:

- SRAM publication: new saves, backup rotation and failed-write preservation.
- Shared catalog: folder/ZIP equivalence, duplicate IDs, hash/capability/target
  rejection, unsafe archives, bounded assets, cache reuse/change/removal/tampering.
  Filesystem symlink escape test skips when Windows lacks symlink privilege.
- F-Zero: 13 C tests; all 75 course record hashes unchanged; folder and ZIP
  catalogs identical; races started in CGP, Astra, MAX and Bower. Synthetic MSU
  mixer checks cover installed soundtracks, same-filename Astra/Bower songs,
  missing-song fallback and Practice. Raw FZEdit decoding matches the old loader
  through folder and wrapped ZIP installations. Astra completed-cup records,
  details, vehicle isolation and battery reload pass on both game engines.
- SMW: 49 semantic importer/reader tests; 18 old/folder/ZIP comparison runs.
  BMP, WRAM, VRAM and CGRAM match the original AIO build byte for byte for
  stock, installed-but-unselected, title selection, stages 001/00F and Luigi.
  Save namespaces remain isolated. The original worktree stays unchanged.

No game assets, source ROMs, recorded music or semantic DAT payloads are included
in the framework. Pack presentation and gameplay semantics remain game-owned.

## Merge compatibility audit

- The CMake helper alone adds no target, C++ compiler, library lookup or runtime
  behavior. Games must call `snesrecomp_target_data_packs` explicitly.
- Legacy SRAM writers retain their original implementation. Only explicit
  `RtlTryWriteSram` callers use durable temporary-file publication.
- Keyboard defaults remain C/V unless the target defines overrides; MSU keeps
  its ordinary filenames unless the host registers a resolver.
- Interpreter changes only correct decoding after an explicitly registered
  hook redirects the PC. Ordinary execution without redirects is unchanged.
- Collection helpers, resource-path lookup and interpreted-program selection
  are new callable APIs. They are not activated automatically.
- Framebuffer and PPU captures require explicit host calls and environment
  settings. The existing standalone frame dumper gains no game-global linkage.
