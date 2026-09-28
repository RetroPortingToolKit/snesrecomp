# Disassembly-backed coverage study — 2026-09-28

Status: the owner reports all three titles passed manual gameplay review,
including Super Metroid after the Ceres Ridley return fix below. The owner
authorized committing this work and selected Mega Man X for the next study.
Super Metroid retains a separate audio-handshake timing discrepancy; manual
acceptance does not turn that strict comparison into a pass. These are local
investigation commits; merge, release and permanent title integration remain pending.

All three pinned disassemblies assemble byte-for-byte to the complete locally
owned ROMs. [Reference revisions and reproduction](../DISASSEMBLY_AUTHORITY.md)
describe the independent decode checks and their limits.
[Machine-readable results](disassembly-2026-09-28.json) include executable
identities, evidence hashes, capture totals and local artifact paths.

## Outcomes

| Title | Emitted variants audited, before → after | Decode conflicts, before → after | Unique ROM instructions matching disassembly, before → after | 6,000-frame attract | 6,000-frame basic gameplay |
|---|---:|---:|---:|---|---|
| Super Mario World | 2,105 → 3,426 | 32 → 0 | 59,979 → 59,707 | Exact recorded evidence; interpreted work unchanged | Exact recorded evidence; interpreted work unchanged |
| A Link to the Past (USA) | 3,923 → 6,217 | 110 → 0 | 112,205 → 113,358 | Exact recorded evidence; 7.55% fewer interpreted instructions | Exact recorded evidence; 0.28% more interpreted instructions |
| Super Metroid | 4,736 → 14,621 | 570 → 0 | 56,610 → 103,624 | Exact recorded evidence; 55.62% fewer interpreted instructions | Audio-handshake mismatch; reduction remains unqualified |

Variants include distinct entry M/X states and mirrored addresses; they are not
unique functions. More variants do not automatically imply more ROM coverage or
less interpreter work. SMW rejects unsafe decode paths, so its unique matched
ROM instruction count decreases despite more valid entry variants. SMW also has
one byte-guarded WRAM routine outside ROM authority. HLE bodies are reported
separately as unobserved by the ROM decoder, not silently counted as verified.

Each route used identical input scripts and SRAM bytes for the previous-root
control and expanded build, both built against the same corrected runtime.
Evidence comprises frame-by-frame WRAM changes and final three full WRAM,
presented-pixel, VRAM, CGRAM and PPU snapshots. Super Metroid additionally logs
the CPU register file each frame. Runs advance continuously; none were paused
or stepped. These comparisons are finite route evidence, not a whole-game or
independent hardware proof. Audio waveforms were not compared.

## What actually caused failures

| Failure class | Evidence and correction |
|---|---|
| Discovery errors | Wrong-width decode, operand entries and data execution rejected by byte-exact authority; all expanded audited ROM decodes have zero conflicts. |
| Analysis error | Exit-mode propagation mutated previous-round facts and could fail to converge. The fixed-point solver now reads a stable previous round. |
| Host contract regression | Authority-only mirror cfgs shadowed original audio-upload/scheduler hooks. Import now attaches constraints to the existing proven mirror owner. |
| AOT bank semantics | JSL pushed a generated bank; short calls, fallback continuations and yields could replace live PBR. They now preserve the architectural bank; explicit long transfers still select their operand bank. |
| AOT clock selection | A low-bank generated body executing through FastROM used the wrong code-region speed. Cycle weighting now uses live PBR and MEMSEL. The underlying aggregate cycle model remains approximate. |
| Existing interpreter scheduler bug | Both SM control and expanded builds froze at a nested message-box NMI counter wait around frame 2,457. Secondary waits now unwind to the scheduler while retaining the guest stack; both complete the same 6,000-frame gameplay route. |
| Capture identity error | SM's 3 MiB source image was hashed as its 4 MiB bus allocation. Identity now hashes the headerless source image. |
| Validation error | A frozen guest could render to the frame limit and exit zero. Replay validation now rejects interpreter-cap and scheduler-bail diagnostics even with matching dumps. |

These findings distinguish bad discovery/promotion from emitted-code semantics,
host contracts, pre-existing interpreter behavior, and inadequate validation.
They do not justify blanket promotion of every observed address.

## Remaining Super Metroid discrepancy

The final basic route has identical CPU register fields on all 6,000 frames,
identical final three video/resource/full-WRAM snapshots, and matching WRAM
outside nine sound-handshake bytes. Those nine bytes differ on 42 frames:
`$0649–$064B`, `$064D–$064F`, `$0650–$0652`. The disassembly identifies these as
sound-library state, current command and clear-delay fields.

All 79 nonzero current-sound events (14, 43 and 22 across the three libraries)
match in value **and frame**. Queue contents and indices also match every frame.
This narrows the mismatch to acknowledgment/clear timing; it does not establish
identical audio output. The strict comparison remains failed. Raw interpreter
counts are 9,185,071 → 5,020,876, but that reduction is not replay-qualified.
Follow-up: **beads-8wg.2.88**. A prior same-build control repeat matched exactly.

Both builds also retain the existing `$88:D865` truncated inline-argument
wrapper diagnostic. The disassembly locates its four data bytes at `$D869–$D86C`
and real RTL at `$D86D`; the cfg ends at `$D869`. This shared diagnostic has not
been suppressed or classified as a newly introduced promotion failure.

## Capture and regression checks

All twelve final runs completed with zero capture/ingest warnings and no
interpreter cap or scheduler bail. Checkpoint plus journal ingest succeeded for
every run; ingesting both twice produced identical discoveries and instruction
costs. Each captured executable digest matches the actual tested binary.

Validation: 468 Python tests, 88 native-library and 6 native-binary tests, and
targeted C harnesses passed. The bridge harness passed 127 checks, including
nested 8/16-bit counter waits, retained PHP/JSL frames and resumed PLP/RTL.
Executed emitted-C regressions cover live-bank JSL frames, short calls through
AOT and LLE, wait/deadline continuations, and FastROM selection. ROM identity
checks cover 512 KiB/3 MiB images with and without copier headers.

## Owner playtest and integration

### Ridley follow-up

The owner's frozen Ceres session was preserved without pausing or stepping the
process: WRAM, CPU and mapped game image plus the coverage journal and log are
under `build/authority-study/sm/owner-ridley-hang/`. The interpreter reached the
ROM's crash loop at `$80:8573` during frame 23,005; the host then stopped guest
execution but continued presenting. This is **beads-8wg.6.10**.

The matching disassembly explicitly documents `$A6:CC7D` discarding its own
return frame with `PLA`, then returning from its caller. In the expanded build,
`cpu_resolve_ancestor_skip` mistook the diagnostic `interp@$A6CBE5` scope for a
compiled caller: its seeded S happened to match the manually constructed frame.
It returned `SKIP_1`, losing the popped `$A6:CBE8` continuation and resuming at
`$A6:CC44`. That skipped the real segment-update/PLB epilogue and corrupted the
next return. The old coverage control completed the same focused test.

Interpreter attribution scopes now explicitly exclude themselves from compiled
ancestor return resolution. The normal bridge continuation mechanism then
resumes at the actual popped address. No ROM addresses, coverage exclusions,
dummy returns or instruction-limit increases were added to the fix.

The focused test uses actual generated objects and a reconstructed Ceres roar
transition based on the owner's WRAM. It reproduces the failure without turbo
or the display loop; after the fix, all 131,072 WRAM bytes match the previous-root
control. The production-resolver regression fails when the exclusion is removed.
Both diagnostic modes pass, the bridge harness passes 128 checks, and 470 Python
tests pass. All six existing 6,000-frame routes reproduce their pre-fix expanded
candidate evidence and instruction counts exactly
(`build/authority-study/ridley-regression-results.json`).

A fresh-file, controller-only route reaches Ceres Ridley, advances through real
retreat and activates the self-destruct sequence (`$A6:AA50`); the longer idle
continuation reaches the escape timeout/game-over normally. It has no interpreter
cap or scheduler bailout. A separate APU guest-clock synchronization timeout
appears during the intro and remains tracked under **beads-8wg.2.88**. The replay
tool now rejects that diagnostic explicitly; this longer route is not being
declared strictly equivalent or audio-correct.

Final normal/turbo checks each run 16,244 frames. Both accelerate the intro;
normal pacing resumes at frame 10,231 for all navigation and the fight, while
the other run keeps the actual Turbo state enabled. All recorded CPU/WRAM and
scene evidence, and the 14,980,387 interpreted instructions, match. Both progress
beyond the reported hang into Ridley's getaway; neither caps or bails. Each
retains the same one intro APU warning. The executable matching these tests
(`4573fdef651ce538ea7661933215c8c84669f8cfcbad52f9b6634db48ab6f33a`)
was relaunched for owner retest. The owner subsequently reported Super Metroid
good and authorized committing the engine and three title worktrees. The frozen
process was closed gracefully after preserving its evidence. No merge or release
has been performed.

Title worktrees are `_wt-authority-smw`, `_wt-authority-zelda`, and
`_wt-authority-sm` on `feat/authority-correctness`; engine changes are in
`_wt-coverage-feedback` on `feat/coverage-feedback`. Generated control and
expanded sources are isolated under `build/authority-study/<title>/`.

Playtest profiles are copies of existing settings and SRAM. SMW uses its
required adaptive-renderer launcher with screen-based spawning enabled.
All three export fresh coverage journals during owner gameplay. The launch
record is `build/authority-study/playtest-launch.json`, including exact binary
hashes, profile paths, logs and process IDs. Permanent regeneration wiring and
dependency pins must be included when integrating approved title candidates;
the experimental generated trees and owned ROM/reference assets are not commits.

Beads findings are saved centrally. Remote Dolt push remains unavailable because
the configured remote data ref cannot be found; local issue updates succeeded.
