# Collection catalog and guarded patch imports (experimental)

For runtime folder/ZIP installation use [DATA_PACKS.md](DATA_PACKS.md). The
INI format below is the lower-level reviewed donor/import format used by
F-Zero tooling; it is not a second runtime ZIP envelope.

`runner/src/content_pack.h` is a host-neutral catalog and guarded IPS/BPS loader.
It deliberately does not merge cartridge address spaces or infer which changed
bytes belong to a course. A pack is one independently supplied resource with a
game-specific adapter, named collections (cups), and named entries (tracks).
F-Zero's prototype is the first consumer.

## Identity and composition

- Persist `pack-id/cup-id` and `pack-id/track-id`, never catalog indexes.
- The catalog sorts by pack ID. Addition cannot rename or replace an entry.
- Duplicate pack IDs are rejected. File-discovery hosts should quarantine all
  ambiguous external definitions, rather than selecting by enumeration order.
- A cup may describe one track or many. There is no five-track assumption in
  the framework. Track IDs are unique within their pack; every cup must have
  at least one track and every track must refer to a cup in its pack.
- Availability is a host concern: a supplied patch, build capability, enabled
  flag, and game-adapter support determine which collections enter a menu.
- A missing persistent key fails resolution. Do not replace it with the entry
  now occupying its former array index.

## Manifest v1

The format is UTF-8 `key=value` lines, with `#` comment lines. Names cannot
contain control characters or `|`. Unknown fields, repeated scalar fields,
invalid references, overflow, and duplicate cup/track IDs fail the whole
manifest. Each scalar below is required. IDs use lowercase ASCII letters,
digits and hyphens, up to 47 bytes.

```ini
format=1
id=my-single-course
name=My Single Course
author=Track author
adapter=game-specific-adapter-v1
source_sha256=<64 lowercase hex digits>
target_sha256=<64 lowercase hex digits>
cup=bonus|Bonus Cup|0
track=circuit|My Circuit|bonus|2
```

An optional repeated `alternate_target_sha256=<64 hex digits>` accepts up to
eight additional exact donor images. The host must establish equivalent
resource semantics before listing them; an alternate hash is not permission
to execute arbitrary replacement code. Source identity is still required.

The last field of a cup/track is an adapter-local guest slot (0–255). It is
metadata, never a framework dispatch address. A manifest author must verify
the adapter contract; a patch format cannot prove compatibility with a host's
game-specific renderer, scheduler, or course-loading code.

Limits are explicit: 64 packs, 32 cups and 128 tracks per pack; strings and
manifest lines are bounded. The catalog does not impose an installed-pack
order on the host's display order. Built-in entries may be registered through
`cp_catalog_add`; disk manifests use `cp_manifest_read` first. Catalog copies
own their data until `cp_catalog_free`. Failed parsing/addition leaves existing
objects intact.

## Patch execution

`cp_patch_apply` detects format from magic, supports IPS literals/RLE/EOF size
extensions and all four BPS commands (including overlapping TargetCopy), and
checks all three BPS CRCs. It rejects truncated streams, bad ranges and images
larger than 16 MiB. Source bytes and output parameters remain unchanged on
failure; successful output belongs to the caller (`free`).

Use `cp_pack_apply` at the trust boundary. It additionally checks the source
and complete output SHA-256 against the manifest. IPS has no source identity,
so its file extension or apparent successful application is insufficient.
Different encodings of the same output, including IPS versus BPS, are accepted.
Apply every pack to its declared immutable base, never to a previous pack's
patched image.

Additive resource consumers should decode and validate typed resources, discard
the donor image and keep their canonical engine and shared hooks. F-Zero's
course library follows this model. The framework does not infer resource
ownership from changed ROM bytes.

Before executing code-changing content without a matching native module,
`cpu_select_interpreted_program()` installs an intentionally empty dispatch
table. `cpu_select_program(NULL, ...)` means **restore stock**, not disable AOT.
Game-specific native hooks also need to be disabled or separately qualified.
Program selection remains a launch-time operation, never a mid-frame switch.

Link `content_pack.c`, `content_patch.c`, and the existing `sha256.c`. These
modules do not depend on SDL, the mod runtime, ROMs, or a generated program.

## Validation

```sh
cmake -S tests/content_pack -B build-content-packs
cmake --build build-content-packs
ctest --test-dir build-content-packs --output-on-failure
```

The standalone suite covers sparse collections, stable keys after reordering,
duplicate rejection, transactional malformed manifests, IPS expansion/RLE/
truncation, BPS relative copies/overlap, and byte-by-byte CRC corruption.
`tests/runtime_dispatch/known_lle_entry_test.c` separately checks the empty
program's isolation from both stock and alternate compiled dispatch tables.
