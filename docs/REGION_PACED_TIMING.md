# Opt-in region-paced instruction timing

`generate --instruction-blocks --region-paced` enables a conservative timing
path for hosts that use the interpreter bridge as an instruction-boundary
scheduler with an armed master-clock deadline. The runtime's deadline guard
requires an active owning scheduler/bounce context; these flags do not retrofit
that ownership into a host that calls compiled routines directly.
Both flags default to false. SA-1 retains its existing instruction-block
behavior. The options participate in generation-recipe identity, output-cache
identity, and per-bank cache identity; a tree emitted under one setting cannot
be silently reused under the other.

```text
python snesrecomp_cli.py generate --rom owner.sfc --cfg-dir recomp --out-dir private-gen --instruction-blocks --region-paced
```

A project recipe may declare `instruction_blocks = true` and
`region_paced = true` in `generate.toml`. Region pacing requires instruction
blocks explicitly. This changes emission and timing; it does not prove a
routine safe to compile or turn off interpreter fallback.

## Clock contract

Each supported instruction charges its ordinary CPU-cycle count. Master-clock
accounting prices each opcode/operand fetch byte through `cpu_region_speed`
using the live program bank, prices internal cycles at six clocks, and uses
the existing paced helpers for architectural data and stack transfers. Fetch
values come from the materialized instruction bytes, without a duplicate ROM
read, and update the open-bus latch before data accesses. Dynamic direct-page,
index/page-cross and taken-branch additions use six master clocks per internal
cycle. A proven taken conditional branch still pays its extra cycle when
constant-Z folding lowers it to an unconditional IR transfer.

An emission-time byte-access count must agree with the instruction's supported
bus recipe. If a lowering changes that count, the instruction remains
interpreted. Prologue reads that sample the paired host-return frame are ABI
bookkeeping and stay unpaced.

Every region-mode fetch/internal/dynamic addition and each paced data helper
uses `cpu_add_aot_master_cycles`. That API updates the master clock and invokes
the nullable `g_cpu_aot_master_credit_hook`. The active interpreter bridge
installs a callback that queues native instruction time in the existing APU
pending accumulator under the same scheduling predicate as interpretation.
The callback ignores calls outside an active bridge, missing machines,
shared-clock co-sim, and non-extended absolute ownership. Extended absolute
mode retains a pending marker for scheduled syncs; the flush's existing absolute
branch clears it without adding relative catch-up. Crediting itself does not
flush the APU midway through an instruction.

Credits originate at actual increments. They are never inferred from elapsed
time around a call, which could include nested interpretation, nested native
execution, DRAM refresh, DMA, or parked time. Existing interpreter opcode
credit remains separate. The pending accumulator already belongs to bridge
rollback state; the callback adds no independent clock counter to save.

The separate nullable `g_cpu_aot_instruction_boundary_hook` observes completion
of the preceding instruction. Paced labels call it after refresh finalization
and before the deadline check. Paired returns with a nonempty native scheduling marker also call it after
finalizing refresh, covering a last native return instruction without another
label. An aggregate body without origin credit leaves this path inactive.
The bridge applies its existing pending-clock batch threshold there, synchronizes
the beam, and flushes the existing accumulator. It adds no credit. Flushing in
the per-access credit callback would run the SPC midway through an instruction;
omitting boundary flushes can defer a logged SPC event into the next host frame.

## Fallback and ownership

The initial native-mode implementation supports ordinary immediate/implied,
direct-page/absolute/long memory, stack, direct call/return and branch paths.
It falls back before charging the current instruction for:

- Emulation-mode entry and `XCE`, which can change emulation mode mid-body.
- Indirect dispatches, `PEI`, software interrupts/`RTI`, block moves,
  waits/stops, live operands, or an unrecognized bus recipe.
- Effective addresses in system-bank MMIO windows, unusual stack windows,
  and word accesses crossing a bank boundary.

The optional cached-pointer recipe supports load/store and accumulator ALU
operations through `(dp)`, `(dp,X)`, `(dp),Y`, `[dp]`, `[dp],Y` and `(sr,S),Y`.
It first requires the entire pointer in plain low WRAM and previews that host
memory without a CPU bus access. Effective MMIO targets and word bank crossings
fall back before guest changes. Accepted instructions fetch the architectural
pointer exactly once through paced bus helpers, cache it, and preserve full
24-bit indexing carry. Dynamic dispatch, live operands and other pointer
operations remain interpreted. The [pointer fixture](../tests/v2/test_region_pointers.py)
checks all six modes against the actual interpreter, including refusal paths.

MMIO guards do not access a device register to decide whether to fall back.
The pointer preview does not change open bus, clocks or memory. This preserves
side effects and leaves unmodeled device-access timing to the interpreter.

These guards use `interp_tier_dispatch_tail`. When an interpreter owns the
active AOT bounce, that helper unwinds to that owner and resumes the exact guest
PC with registers and stack intact. Starting a new balanced interpreter at a
mid-body guard can consume the wrong guest continuation. Without an active
owner, the helper retains its established balanced fallback.

A JSL envelope restores its caller's bank on ordinary `NORMAL`/`SKIP_N` returns.
During a pending LLE yield/deadline unwind it preserves the live resume bank
through every ancestor. `interp_bridge_lle_unwind_pending()` identifies that
ownership state directly; the emitter does not guess from a sentinel value
that decreases as host frames unwind.

## Evidence and limits

The ROM-free [region fixture](../tests/v2/test_region_pacing.py) executes emitted
C against the actual interpreter core and current runtime paced-helper
definitions. It compares CPU/master totals, registers, flags, stack, memory,
and open bus across slow/FastROM cases, plus credit-policy and ownership guards.
The [deadline fixture](../tests/v2/test_instruction_block_deadline.py) demonstrates
aggregate-block overshoot, and the [JSL fixture](../tests/v2/test_jsl_live_pb.py)
checks live-bank preservation through nested unwinds.

These are bounded synthetic checks, not hardware, campaign, performance, or
gameplay qualification. Instruction-boundary agreement does not establish
per-bus-phase scheduling or device-state equivalence. A game must retain its
code-patch guards and interpreter policies, then compare matched-input native
and interpreter execution, guest memory, devices and rendered output before
claiming route qualification. See upstream [timing issue 205](https://github.com/RetroPortingToolKit/snesrecomp/issues/205)
for the associated timing investigation.

## Netplay region agreement

The console region is part of a netplay match's compatibility identity. NTSC
and PAL run the same guest at different master clocks and field geometry (262
vs 312 scanlines), so two peers that load the same ROM but resolve different
regions cannot stay in sync while presenting the same ROM identity.

The agreed policy:

- With auto, each peer resolves its region from the cartridge's `$FFD9`
  destination code, or from `SNESRECOMP_REGION` when that is set.
- An explicit `--region` / `snes_set_region_override` is per-peer and must
  resolve to the same region on every peer. Overriding to the region the header
  already selected does not change the identity.
- A mismatch is a hard refusal before frame 0, never a silent correction. Both
  peers report the reason `region_mismatch` with a player-facing message naming
  each side's resolved region and its source (`override`,
  `SNESRECOMP_REGION` or `header`), then soft-return to the waiting room.

The desktop host publishes its resolved region after `SnesInit` and before
`snes_netplay_start`. `snes_netplay_rb_set_identity` folds that region into the
content fingerprint passed to recomp-net's `rnet_rb_driver_set_identity`, so
the existing identity gate compares it. `snes_netplay_poll_admit` checks
agreement before the first admit; a mismatch raises the dedicated local reason
and the return-to-lobby request that `snes_host_barrier_admit` consumes. The
player-facing message reaches `snes_host_lobby_set_runtime_error`.

This folding is a stopgap entirely inside snesrecomp. It deliberately does not
touch the shared, currently uninitialized `lib/recomp-net` submodule. The
fingerprint carries the region bit; the source label is published through
`snes_netplay_rb_set_peer_identity` when the driver exposes the peer identity,
and the refusal copy names it when present. A dedicated region field in
recomp-net's identity exchange would carry each side's region and source
explicitly instead of encoding the region in a fingerprint and relying on the
driver's equality gate; that cleaner design is pending reviewer preference.

The ROM-free [region agreement fixture](../tests/v2/test_region_agreement.py)
drives both peers through the real barrier with one synthetic ROM: one PAL from
an override, one NTSC from its header. It checks the refusal, zero admits and
zero guest frames, the message on both sides, and a matching-region control
that still admits. The dependency-light core fixture pins the fold and the
verdict where the recomp-net headers are unavailable.
