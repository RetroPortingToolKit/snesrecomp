# Native whole-program analyzer

This crate is snesrecomp's whole-program analyzer. It emits the stable
format-3 `ProgramManifest`; the Python emitter consumes that manifest and
remains responsible for generated C.

It is the only analyzer. `tools/v2_emit.py` (and `snesrecomp_cli.py generate`)
build the release executable on first use when it is missing, and fail loudly
if it cannot be built or rejects an input; nothing falls back to a different
analyzer. `--analysis-backend auto|native` is still accepted and means native;
`python` is an error.

## Build

Install rustup, then run from the repository root. The checked-in toolchain
file selects the same Rust version and components used by CI:

```sh
python tools/build_native_analyzer.py --test
```

The helper performs locked release builds and produces:

- Windows: `recompiler-rs/target/release/snesrecomp-analyze.exe`
- Linux/macOS: `recompiler-rs/target/release/snesrecomp-analyze`

CI builds and tests Windows x86-64, Linux x86-64, and macOS arm64 executables.
Each workflow run publishes downloadable archives; published GitHub releases
receive the same archives as release assets. A downloaded binary can live
anywhere when `SNESRECOMP_NATIVE_ANALYZER` points to it; a path named that way
must exist and is never rebuilt.

## Use

```sh
python tools/v2_emit.py --rom game.sfc --cfg-dir recomp \
  --out-dir src/gen --cfg-roots
```

`--max-insns` and `--max-nodes` are passed through to the analyzer.

`tools/v2_compare_analysis.py` compares two manifests at the emission
compatibility boundary, for checking an analyzer change against a baseline;
`--strict-summaries` also compares diagnostic graph details.

## History

The native analyzer replaced the original Python analyzer. On the 2026-07-18
Mega Man X static-coverage workload the Python analysis took 402.5 seconds and
the native analysis 14.6-15.7 seconds (25.7-27.6x faster), producing the same
4,561 variants, exit facts and AOT/LLE split; driving the emitter with either
manifest produced byte-identical C. Super Metroid was 48.2 s against 2.6 s.

The Python analyzer stayed as an `auto`-mode fallback for the rollout. It was
retired on 2026-09-24: every title regenerates with the native analyzer, the
fallback had drifted (it lacked rules the native analyzer gained, so a checkout
without the binary silently produced a different, less safe program), and a
second analyzer made the generated code depend on whether the binary happened
to be built. Its manifest-level regressions were ported to run against this
analyzer (`tests/v2/test_analysis_tool.py`, `test_program_analysis.py`) and its
exit-equation solver tests to this crate's unit tests.

Replacing the Python emitter is a separate project and should require
generated-C and gameplay equivalence gates.
