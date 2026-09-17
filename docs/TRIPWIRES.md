# Runtime tripwires

snesrecomp's recompiled binary ships with a set of always-on runtime
tripwires that compare the decoder's static expectations against the
actual runtime state and latch on the first divergence. Each tripwire
is auto-armed at boot and exposes its state over TCP for queries.

The tripwires exist to **short-circuit the "user reports visible bug
→ we diagnose for hours" loop**. Any silent-but-real decoder gap
that fires the tripwire surfaces as a structured TCP message with
full state capture (frame, PC, CPU registers, recomp call stack),
*before* the divergence cascades into visible damage.

## Active tripwires

### M/X claim verifier — `mx_claim_check_*`

Catches: the decoder's static `(m, x)` claim at a function entry
doesn't match runtime `cpu->m_flag` / `cpu->x_flag`. The claim is
encoded in the function-variant name suffix (`_M{m}X{x}`) so the
verifier reads it for free.

Fires at: every `cpu_trace_func_entry` (every recompiled function
invocation).

Catches the class of bug: function variant mis-routed at the
caller's emit site — the decoder thought the callee would be
invoked at one (m, x) state, runtime arrived at a different one.
The 2026-05-16 `BufferScrollingTiles_Layer2_Init_M0X0` and
`RunPlayerBlockCode_00F28C_M0X1` trips were both this class; both
closed by the autoroute fixpoint fix in snesrecomp 808e918.

TCP:
- `mx_claim_check_arm` — arm + clear trip
- `mx_claim_check_get` — JSON snapshot (armed, triggered, frame,
  pc24, claimed/runtime (m, x), full CpuState, recomp stack)
- `mx_claim_check_disarm` — disarm

### Async m_flag / x_flag write tripwire — `mx_async_check_*`

Catches: `cpu->m_flag` or `cpu->x_flag` mutates between two
checkpoints **without** going through any emitted SEP/REP/PHP/PLP/
RTI/XCE (i.e. an *asynchronous* writer). The DA49 entry in
`SuperMarioWorldRecomp/ISSUES.md` describes the canonical case:
NMI/IRQ state restoration is the prime suspect for SMW.

Mechanism: every legitimate emit-side flag change calls
`cpu_trace_px_record`, which increments a global
`g_px_mutation_count`. The tripwire snapshots
`(m_flag, x_flag, g_px_mutation_count)` at every `cpu_trace_block`
hook (basic-block granularity). If flags changed but count didn't
between snapshots, an unexpected writer ran.

Fires at: every `cpu_trace_block` (every basic block in
recompiled code).

TCP:
- `mx_async_check_arm` — arm + clear trip
- `mx_async_check_get` — JSON snapshot (armed, triggered,
  px_mutation_count, frame, block_pc24, prev/new (m, x), recomp
  stack at trip)
- `mx_async_check_disarm` — disarm

### DB tripwire — `db_tripwire_*`

Catches: `cpu->DB` transitions to a specific target bank. Used for
narrowing wrapper-bypass bugs where DB stays at the caller's bank
instead of transitioning at a PHB/PHK/PLB wrapper.

(Older tripwire — see `cpu_trace.h` / commit log for usage.)

### Stack-drift tripwire — `stack_drift_*`

Catches: the runtime stack pointer's delta across a function
doesn't match the decoder's expected delta. Identifies functions
with imbalanced PHA/PLA or PHB/PLB / unexpected non-local-return
patterns.

### Phantom-PC trap

Catches: runtime CPU lands at a PC that wasn't decoded as a
function entry. Symptom of misdecoded dispatch tables or wrapper
bypass.

(Always-on, no arm/disarm — fatal on hit since it indicates
runtime jumped to nonexistent code.)

### WRAM watch slots — `block_watch_*`, `wram_watch_*`

Catches: writes to specific WRAM addresses (manually armed). Used
for narrowing "what writes $7E:XXXX with this value at this
frame?" investigations.

### Off-rails detector — `offrails_get`

Catches: impossible CPU paths — `RomPtr` called with addresses
outside valid ROM space (`< $8000` or `>= $7E0000`), cart reads
out of range, and similar soft-fail probes from the framework.
Each `(tag, high-16-bits-of-hint)` combination is a bucket; first
hit per bucket emits a single-line stderr message and captures
context (frame, hint, recomp stack top). Repeats accumulate
silently into `hit_count` + `last_frame` / `last_hint`.

Replaces the prior multi-thousand-line stderr dump (1024 DB/PB
mutations + 64 trace events per first hit) that caused
multi-second mid-gameplay stalls while stderr drained. The same
data is preserved per-bucket and reachable via the TCP query.

TCP:
- `offrails_get` — JSON dump of every bucket (tag, first/last
  frame, first/last hint, hit_count, stack_top).

No arm/disarm — the detector is always live; the buckets are
allocated as events fire.

## Tripwire framework — how to add a new one

A new tripwire follows the same shape across all of them:

1. **Decide what to latch on.** A *single* binary condition.
   Tripwires are one-shot for clarity; if a condition fires
   multiple times per run, the first instance is the most
   informative.

2. **Add the storage type to `cpu_trace.h`.** Mirror
   `MxClaimViolation` or `MxAsyncTrip`: `armed`, `triggered`, the
   captured context (frame, PC, register snapshot, recomp stack).

3. **Add the API to `cpu_trace.h`**: `cpu_trace_arm_X`,
   `cpu_trace_disarm_X`, `cpu_trace_X_check`. Provide non-trace
   inline stubs for the `#else` block.

4. **Add the implementation to `cpu_trace.c`.** The check function
   is called from a hot-path hook (`cpu_trace_block`,
   `cpu_trace_func_entry`, `cpu_trace_db_change`, etc.). Keep the
   check cheap — early-exit on `!armed || triggered`. On trip,
   capture the recomp call stack via `g_recomp_stack` and the
   relevant CPU registers, log to stderr with a `[X]` tag.

5. **Auto-arm at boot.** Add an `cpu_trace_arm_X()` call alongside
   the existing arms in `cpu_trace.c`'s init path.

6. **Expose via TCP.** Add `cmd_X_arm`, `cmd_X_disarm`, `cmd_X_get`
   handlers in `debug_server.c` (mirror the M/X claim ones) and
   register them in the dispatch table near the bottom of the file.

7. **Document here.** Add an entry to this file's "Active
   tripwires" section explaining what it catches and what TCP
   commands query it.

## Querying tripwires

A minimal Python TCP client was kept at
`SuperMarioWorldRecomp/_triage/probe_mx_claim.py`. That file is no longer in
that checkout, so write the three lines yourself rather than hunting for it --
the protocol is one command per line, reply as JSON. Pattern:

```python
python _triage/probe_mx_claim.py mx_async_check_get
```

Returns JSON: `{"armed":1,"triggered":1,"frame":4581,...}`. Empty
`triggered` field on a clean run means the condition never
occurred during the captured window.

## Known follow-ups

- **D-flag async tripwire (deferred)** — same shape as M/X async,
  but requires recompiler-side instrumentation: emit code for
  `SED`, `CLD`, `PLP`, `RTI` needs to call `cpu_trace_px_record`
  (or a parallel `cpu_trace_d_record`) so legitimate D-flag
  mutations can be distinguished from async ones. Currently only
  M/X-affecting emits call `cpu_trace_px_record`.

- **Dispatch handler runtime verifier** — at each dispatch JSL
  site, verify the actual handler reached at runtime is in the
  decoder's enumerated table. Catches dispatch over-decode and
  variant gaps not visible to the M/X claim verifier.

- **C / N / V / Z claim verifier** — generalize the M/X model to
  the other ALU flags. Less useful in practice (those flags change
  every ALU op), but worth considering for specific call-site
  invariants where the decoder believes a flag is statically
  known.
