# Collection catalog (experimental)

For runtime folder/ZIP installation use [DATA_PACKS.md](DATA_PACKS.md). The
INI format below is the lower-level reviewed donor/import format used by
F-Zero tooling; it is not a second runtime ZIP envelope.

`runner/src/content_pack.h` is a host-neutral collection catalog.
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
- Availability is a host concern: supplied data, build capability, enabled
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

## Resource loading

The generic playable-ROM patch workflow is removed. `cp_patch_apply` and
`cp_pack_apply` remain for reviewed donor-resource imports used by F-Zero's
graphics and vehicle loaders. They create a separate disposable image and
leave the original source unchanged. Decode and validate typed resources,
then discard that image; do not install it as a playable cartridge or use it
to select a donor program. Existing BS content and native music integrations
remain game-owned features rather than arbitrary ROM-patch launcher inputs.

Link `content_pack.c`, `content_patch.c` and `sha256.c`; these have no SDL or
generated-program dependency.

## Validation

```sh
cmake -S tests/content_pack -B build-content-packs
cmake --build build-content-packs
ctest --test-dir build-content-packs --output-on-failure
```

The standalone suite covers sparse collections, stable keys after reordering,
duplicate rejection, transactional malformed manifests, IPS expansion/RLE/
truncation, BPS relative copies/overlap and checksum corruption.
`tests/runtime_dispatch/known_lle_entry_test.c` separately checks the empty
program's isolation from both stock and alternate compiled dispatch tables.
