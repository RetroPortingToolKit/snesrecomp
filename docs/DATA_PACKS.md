# Optional data-pack catalog

`snesrecomp_target_data_packs(target)` attaches a C ABI catalog backed by
LibArchive and RapidJSON. It is explicitly opt-in. Merely including
`runner.cmake` adds no pack discovery, dependency, UI, directory or guest
behavior to an existing game.

This extracts the transport/validation pattern used by F-Zero Forever into a
game-independent layer. SMW's `feat/shared-data-packs` is the first consumer.
The existing F-Zero loader has **not** been migrated on its release branch.

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
stock program. The older `content_variant` program/ROM-switch mechanism is a
different API and is not used by this proof.

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

ZIP entries are read in memory, never extracted, so no cache can resurrect a
removed pack. The catalog snapshots payloads for the session. Add/remove/replace
files between launches. Assets read later must remain available for that session.
Install exactly one folder or ZIP for each ID. There is no hot reload.

Limits: 128 candidates, 128 MiB per payload, 512 MiB admitted payloads, 64 KiB
manifest, 32 JSON nesting levels, 20,000 ZIP entries, 2 GiB per ZIP entry and
8 GiB total declared ZIP data. Relative asset reads have a caller-specified
allocation bound. Absolute/traversing paths, external folder symlinks, archive
links, encrypted entries, duplicate paths (case-insensitive), duplicate JSON
keys and Windows device filenames are rejected. LibArchive normalizes legacy
ZIP backslashes before path validation. No path is ever written by this API.

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
qualified bytes and metadata. The writer refuses to overwrite an existing pack.
It does not convert ROMs or qualify mechanics.

## Further reuse

F-Zero can adopt the catalog through an adapter for its existing course index;
its raw FZEdit importer, course hashes, records, title art and per-course music
mapping remain F-Zero code. A future streaming-asset API is needed before moving
F-Zero's large PCM files behind this memory-buffer API. Do not read whole songs
through it or introduce game-specific MSU routing in the shared loader.

Title-art enumeration, presets, conflict descriptions and presentation of pack
errors are plausible future shared metadata. They are not implemented here;
first establish a second real consumer before expanding the schema. Game format
and capability versions can evolve independently of the transport envelope.
