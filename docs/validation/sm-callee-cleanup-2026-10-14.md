# Super Metroid attract freeze: callee-cleaned return frames

The shared compiler fix is `28823fcce1c12140930980b66c0d429e33053298` on
`fix/paired-callee-stack-cleanup`. It fixes the pre-existing long attract-demo
freeze reported during PRs #171–173 qualification. Owner play approval of the
new fix is pending. No Super Metroid runtime override is required.

## Cause and change

`$A0:BB70` consumes two word arguments from the guest stack. It relocates its
three-byte JSL return frame above those four bytes, discards the arguments,
and RTLs to the original paired continuation, `$A3:E8C1`.

The generated return handler required the pre-RTL stack pointer to equal the
function's entry pointer before returning to the native caller. This valid
callee cleanup failed that check. Dispatch then missed the internal return
address and restored S from the correct `$1FEE` to `$1FEA`, losing the cleanup.
The caller subsequently returned through stale bytes. A bounded instruction
probe caught execution at the JSL operand `$A3:E8C0`, decimal mode becoming
set, and the next enemy-pointer addition changing `$E5FF + 0` into `$4665`.
The resulting `$A7:0000` callback entered the ROM crash loop.

For an unbalanced return, known ancestor resolution still runs first. A paired
RTS/RTL can then return normally when the popped PC equals its saved original
continuation and the pre-return S is shallower than entry. This preserves
callee-cleaned arguments. Rewritten PCs, deeper computed-return frames,
unpaired returns and known non-local ancestors retain their existing paths.

An execution test compiles generated C for both RTS and RTL cleanup, stack
wrap, ancestor priority and a deliberately rewritten continuation. It fails
on the old compiler with `S=1FEA expected=1FEE` and passes with the fix.
Temporary probes and experimental game-side tail-dispatch changes were removed.

## Game validation

Both the original main baseline and PR integration candidate freeze at frame
12,910 of a 20,000-frame cold-boot attract run, with 96 interpreter caps and
one scheduler bailout each. Host frame counts continue after the guest stops.

The unchanged Super Metroid title sources with the compiler fix complete
20,000 and 60,000 frames with zero interpreter caps, scheduler bailouts or
APU synchronization timeouts. Final guest NMI counters advance
`19847, 19848, 19849` and `59847, 59848, 59849`, respectively; completion is
therefore not inferred solely from host frame counts.

Freshly regenerated Release builds were also compared against the previously
qualified framework runtime `835c30e4` using the same ROMs, title revisions,
UI revisions, input saves, scripts and silent SDL audio/video drivers in both
arms. All nine routes match every available evidence file exactly:

| Game | Routes | Actual frames per arm | Result |
|---|---|---:|---|
| Super Mario World | Attract, basic | 12,000 | Exact |
| A Link to the Past | Attract, basic | 12,000 | Exact |
| Super Metroid | Attract, basic, Ceres/Ridley | 28,244 | Exact |
| Mega Man X | Attract, Highway | 24,000 | Exact |

Total: 76,244 frames per arm, 152,488 paired frames. Both Ceres runs retain
one existing APU synchronization timeout and quit at scripted frame 16,244;
the 30,000-frame safety cap is not counted as completed work. No paired route
reaches an interpreter cap or scheduler bailout. ALttP's title-specific host
does not emit `cpu.csv`; its WRAM trace and complete scene captures are compared.

A 20,000-frame full-interpreter diagnostic reference matches all sampled CPU
registers and final pixels, VRAM, CGRAM and PPU state. Its WRAM trace differs
in scratch byte `$0002`, retired stack bytes and sound-handshake state.
Whole-state or cross-build equivalence is not claimed. The interpreter-only
setting is a diagnostic reference, not the fix or a new default.

## Checks and evidence

[CI](https://github.com/RetroPortingToolKit/snesrecomp/actions/runs/38671881053)
passes Linux x86-64, Windows x86-64 and macOS arm64. Linux passes 732 Python
tests, the C runtime tests and optional content-pack suites. Each platform
passes 97 Rust library and seven binary tests. Local compiler tests pass all
503 cases. The full Windows Python run has 729 passes, two platform skips
and one CMake-path failure; that remaining test passes with the native MinGW
CMake directory before MSYS shims on PATH.

[Machine-readable evidence](sm-callee-cleanup-2026-10-14.json) records build
identities, generated-input manifest hashes, route results, long-run evidence
hashes and diagnostic-reference differences. Private ROM-derived captures and
replay cases are under `F:/Projects/_regression-review-20261014/`, in
`cleanup-regression/` and `sm-cleanup-attract/`. No ROM or generated C is committed.

Tracking: `beads-8wg.2.126` (shared compiler) and `beads-8wg.6.11` (title symptom).
All future owner and automated launches are muted by default, with audio
emulation retained. This validation is limited to the routes above.
