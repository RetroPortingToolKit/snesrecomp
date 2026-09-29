# Shared source files in the mod launcher

A `[[resource]]` may declare `shared_key = "game.source.rom"`. Resources with
that key use one canonical path across features and packages, even if the other
feature is disabled. Selecting, replacing or clearing either picker immediately
changes the value returned by all consumers. State persists one
`[[shared_resource]]` entry (`id`, `path`), including an explicit empty value.
An older per-feature value migrates when first read if no shared value exists.
Resources without a key retain their existing independent behavior.

`normalized_sha256` optionally verifies the original source file before a
feature can commit. A 512-byte copier header is stripped when the file length
modulo 32768 is 512, matching the SNES runtime's ROM identity convention.
Wrong/missing ROMs display a provider validation error; the launcher does not
ask users to produce extracted caches. Hash checks are memoized by path, length
and modification time to keep redraws inexpensive. Trusted game code performs
its own final validation and extraction before loading imported content.

`tests/mod_resources/shared_source_rom_test.cpp` covers synchronization in both
directions, independent keys, persistence, clearing, copier headers and wrong-ROM
rejection through the real launcher provider. The test uses synthetic bytes.
