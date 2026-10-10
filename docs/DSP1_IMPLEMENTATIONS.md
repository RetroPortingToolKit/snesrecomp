# DSP-1 builds and caller contract

The DSP-1 implementation is fixed when building. `SNESRECOMP_DSP1_IMPL=LLE`
is the default; `SNESRECOMP_DSP1_IMPL=HLE` selects the existing firmware-free
command model. Use distinct build directories. Both expose the same `dsp1.h`
host interface and canonical Super Mario Kart cartridge windows.

```text
cmake -S <title> -B build-dsp1-lle -DSNESRECOMP_DSP1_IMPL=LLE
cmake -S <title> -B build-dsp1-hle -DSNESRECOMP_DSP1_IMPL=HLE
```

The shared `runner/runner.cmake` includes `runner/dsp1_backend.cmake`, validates
the choice and supplies `SNESRECOMP_DSP1_HLE=0` or `1` consistently to consumers.
Standalone non-CMake builds default to LLE; pass the same numeric definition to
all translation units if selecting HLE. The choice is part of the CMake cache
and compile command identity. `dsp1_build_implementation()` and startup output
identify what the binary contains. There is no runtime implementation selector.

LLE maintains the NEC uPD7725 instruction-level core in `runner/src/snes/dsp1.c`.
It requires an external 8192-byte `dsp1b.rom`, `dsp1.rom`, or word-reversed
`dsp1.bin`. `SNESRECOMP_DSP1_ROM` names a firmware input; it never selects HLE.
Existing current-directory, `firmware/`, and ROM-directory searches remain.
Missing firmware leaves LLE inactive and reports the requirement; it does not
substitute HLE. HLE ignores all firmware inputs, even valid files.
`dsp1_load_firmware()` retains its firmware-loaded return convention: it returns
zero in HLE builds, whose readiness is indicated by `dsp1_hle_active()`.

HLE replaces firmware instruction execution with native command computation in
`dsp1_hle.c`, and a host DR/SR protocol adapter in `dsp1.c`. Its supported scope
is the verified command set used by Super Mario Kart. Keep command arguments,
word/byte ordering, returned values, ready/completion behavior, projection state,
reset, and caller-visible errors compatible. Unknown commands stop the HLE
protocol with a failed-command diagnostic instead of fabricating results. The
current implementation retains its existing readiness-delay model; build
selection does not change arithmetic or protocol timing.

Supported disk and memory snapshots use distinct DSP-1 LLE/HLE header identities
in `snapshot_identity.h`. Both loaders reject a different implementation or an
old untagged DSP-1 snapshot before loading guest state. Non-DSP-1 titles retain
their existing header and payload. Native game saves are unaffected. Raw device
`dsp1_saveload()` is an internal same-build serializer; its caller must enforce
the enclosing snapshot identity. Cross-build savestate conversion is unsupported.

## Focused validation

```text
cmake -S tests/dsp1 -B build/dsp1-lle -DSNESRECOMP_DSP1_IMPL=LLE
cmake --build build/dsp1-lle
ctest --test-dir build/dsp1-lle --output-on-failure
cmake -S tests/dsp1 -B build/dsp1-hle -DSNESRECOMP_DSP1_IMPL=HLE
cmake --build build/dsp1-hle
ctest --test-dir build/dsp1-hle --output-on-failure
```

Selection tests use an isolated synthetic firmware fixture solely to verify
loader/backend choice; they do not execute it. Command tests exercise native
computation. HLE host tests cover status/byte sequencing, save/load continuation
mid-raster output, stream termination, and unsupported-command failure. LLE tests
cover instruction-level host registers; firmware differential tests require a
real `SNESRECOMP_DSP1_ROM` and report a skip if unavailable. Header identity tests
check that DSP-1 backends and legacy states differ while non-DSP-1 identity stays
unchanged; title-level disk/memory rejection remains an integration gate.

Existing command and attract qualifications predate explicit build selection.
They are reference evidence, not proof that this wiring has improved performance
or that every gameplay transition is safe. Measure paired same-ROM, same-input
title builds and validate affected race/menu/progression and audio behavior
before promoting HLE. LLE remains the default pending that evidence.
