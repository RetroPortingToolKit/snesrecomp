# Coverage capture and AOT discovery

Coverage Capture records where the interpreter actually executes, why a
transfer fell back, and which exact entry variants merit fresh analysis.
It does not certify that generated code is correct. Promotion requires a
build and a replay comparison, followed by gameplay validation.

## Enable capture

The engine-owned mod `snesrecomp.diagnostics.coverage` is hidden and off by
default. Expose it with `-DSNESRECOMP_EXPOSE_COVERAGE_MOD=ON`, the launch flag
`--expose-coverage-mod`, or this INI setting:

```ini
[Diagnostics]
ExposeCoverageMod=true
```

Exposure only makes the mod available. Select **Coverage Capture** in the
launcher to enable it; the title's mod selection persists in `state.toml`.
This diagnostic selection does not change netplay compatibility or get
adopted from another player. It is an engine feature, not an installable mod.

For automation use `SNESRECOMP_TIER2_CAPTURE=1` or
`--coverage-capture=on`. Explicit off is supported. Precedence, highest first:
launch override, environment override, `[Diagnostics] CoverageCapture`, mod
selection/default. Legacy `SNESRECOMP_TIER2` and an explicit journal path
remain supported. There is no environment lookup per interpreted opcode.
Custom title hosts must integrate the shared configuration/mod hooks to
offer the launcher controls; environment capture also works in headless hosts.

The default Windows export is
`%LOCALAPPDATA%/snesrecomp-coverage/<title>/<session>.json`, accompanied by a
`.jsonl` journal. Set `SNESRECOMP_COVERAGE_DIR` to an existing parent directory
to redirect sessions. `SNESRECOMP_TIER2_MANIFEST` and
`SNESRECOMP_TIER2_JOURNAL` select explicit files; use fresh paths per process
when automating. Rematch/reset archives an explicit checkpoint before starting
a new session. The log prints the destination and which setting enabled it.

The journal is buffered and flushed about once a second. Checkpoints replace
the previous JSON atomically about every five seconds and at orderly shutdown,
reset and disable. A hard process termination can lose recent buffered data.
The final checkpoint contains instruction costs; the journal contains transfer
observations, so a recovered journal alone is not complete cost accounting.

## Read the evidence

```sh
python tools/tier2_ingest.py capture.json capture.jsonl \
  --cfg-dir /path/to/title/recomp \
  --program-manifest /path/to/captured-build/program_manifest.json
```

Add `--json` for a machine-readable audit. Keep the original generated program
manifest with each tested build: the reader verifies its SHA-256 before joining
analysis results. Joining against a later regenerated manifest is rejected.

The v2 identity includes ROM SHA-256, module, mapper, generated program digest,
executable SHA-256 (`build_digest`) and generation digest. Different builds
must be audited separately. Executable identity does not cover external DLLs
or configuration; keep those inputs with the replay artifacts too.

Each transfer key retains processor, raw caller/target PC, M/X, emulation mode,
transfer kind and fallback reason. Only documented mapper aliases are folded
for analysis; SA-1's upper ROM banks are not LoROM mirrors. Outcomes mean:

| Counter | Meaning |
| --- | --- |
| `observed_hits` | Interpreter saw a call/landing; no completion claim |
| `completed_hits` | A tracked bridge entry returned normally |
| `yielded_hits` | Entry yielded to its scheduler |
| `bail_hits` | Execution hit a bridge failure/limit |
| `pending_hits` | A tracked entry has not yet reached an outcome |

Instruction costs are exclusive executed-opcode counts and guest cycles,
keyed by instruction PC, widths and E. Main CPU and SA-1 costs are separate.
They are neither wall-clock speedups nor guessed per-function inclusive costs.
Standalone interpreter hosts can export costs without inventing transfer
boundaries; SimCity currently uses this mode.

Rows are cumulative within a capture. The reader keeps the newest sequence
before summing independent captures. Checkpoint + journal + duplicate bundle
imports are idempotent. A bundle uses schema `snesrecomp tier2 bundle v2` and
a `records` array containing the original records, with IDs and sequences
preserved. Only a torn final journal line is recoverable; malformed complete
records and conflicting equal-sequence rows are errors. Overflow, missing
identity, failed writes and a journal newer than its checkpoint are visible
warnings, not silent success.

## Discover, regenerate, qualify

```sh
python tools/v2_emit.py --rom /path/to/game.sfc --cfg-dir /path/to/recomp \
  --out-dir /path/to/new-gen --cfg-roots --no-host-root-scan \
  --profile-manifest capture.json --profile-manifest capture.jsonl
```

Preserve the title's existing roots and profile inputs when comparing a new
capture; replacing historical coverage with a short attract capture can shrink
the generated program. Do not concatenate captures from different builds to
solve this. Retain reviewed root declarations or analyze the old evidence
separately, then qualify the complete regenerated build.

V1 profiles remain auditable. Generation additionally requires
`--legacy-profile-rom-sha256 <verified-ROM-digest>` because v1 did not bind its
observations to a ROM. The SDK/CLI forwards this flag. Supporting title regen
scripts expose `SNESRECOMP_LEGACY_PROFILE_ROM_SHA256`. This explicit association
does not recover missing E, build identity or completion evidence.

Call observations with exact native-mode widths can seed analysis. Goto/tail
landings need an independently declared function boundary. Bailed variants,
unsafe targets, missing modes and E=1 do not become automatic candidates.
Unsafe targets remain interpreter-only even if another static root reaches
them. Legacy address allowlists restrict candidates; they are not portable
proof across generated builds.

The decoder can recover bounded pointer sets from supported predecessor
instructions, including table/index transformations. Every recovered dispatch
checks the actual runtime pointer and retains an interpreter default. Unknown
joins or clobbers stop recovery. Byte-matched external disassembly can supply
instruction boundaries through `tools/ingest_disassembly_authority.py`, which
checks ROM identity and bytes before creating a new cfg overlay. Neither source
bypasses analysis. SA-1 generated bodies guard each instruction fetch against
the live mapper, including instructions after an MMC write.

RAM snapshots preserve entry mode and captured bytes. Offline decoding follows
control flow to find a valid extent; an RTS-shaped operand is not an end marker.
Truncation, escapes or changing snapshots block promotion. Runtime stability
currently hashes the entire bounded snapshot, so changing non-code tail bytes
can conservatively reject an otherwise stable routine. Guarded materialization
and replay remain necessary.

## Reproduce a gameplay route

`tools/coverage_replay.py` executes a local JSON case in a new directory:

```json
{
  "route": {"title": "example", "route": "attract", "frames": 6000},
  "command": ["/path/to/headless.exe", "/path/to/game.sfc", "6000"],
  "env": {"SNESRECOMP_SAVE_ROOT": "saves",
          "SNESRECOMP_WRAM_DUMP": "{run}/wram.bin"},
  "evidence": ["wram.bin"],
  "timeout": 180
}
```

Use the title's actual command-line/input/dump interface. Optional `copies`
maps source paths to relative destinations (e.g. an SRM to `saves/save.srm`);
`files` creates route/config text. `{run}` expands to the isolated directory.
`activity_minimums` maps host log counters (for example `video_changes`) to
minimum values. These counters must be present, meet their minimums and match
the reference. This catches two runs that end on the same black frame after
following different execution paths. Always include guest state in `evidence`.
`capture: false` plus `rom_sha256` enables a capture-off control. Cases execute
argv directly without a shell; originals and player saves are never modified.
Use a short relative save root: older runtime save APIs truncate roots longer
than 95 characters. A relative `saves` is inside this case's working directory.
For hosts that patch the ROM before loading it, compare against the loaded
image's digest; SimCity's existing cursor/View patches are one such example.

```sh
python tools/coverage_replay.py route.json runs/baseline
python tools/coverage_replay.py candidate-route.json runs/candidate \
  --reference runs/baseline/result.json
```

The output includes the process log, checkpoint/journal, audit, evidence hashes
and result JSON. A comparison requires identical route/input/SRAM identity and
ROM, successful exits, matching nonempty evidence and no capture warnings.
Only then are interpreter-work reductions reported, separately per processor.
Rejected comparisons retain raw counts but leave reduction percentages null.
Use state traces plus image/audio evidence when the host provides them: one
final screenshot cannot prove the whole route. Basic fuzzing supplements an
attract route; inspect images to confirm the inputs reached meaningful play.
Passing these routes is limited evidence, not proof of every game state.

The shared desktop host accepts `turbo on` and `turbo off` in input scripts.
They change the same held-Turbo state as the keyboard at the current frame
boundary, without adding a guest frame. This exercises turbo's presentation
skipping as well as its pacing; `DisableFrameDelay=1` alone does not. Interpreter
caps, scheduler bailouts and APU guest-clock synchronization timeouts disqualify
a replay even when the host exits successfully.

The [disassembly authority workflow](DISASSEMBLY_AUTHORITY.md) adds byte-verified
discovery constraints and audits the actual emitted instruction boundaries.
Capture identity uses the normalized source image before power-of-two bus
mirroring, so a 3 MiB ROM's logs can be ingested against that same 3 MiB ROM.
