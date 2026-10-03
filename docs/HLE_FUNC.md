# Reimplementing a routine in C: `hle_func`

`hle_func` replaces one guest routine with a C function chosen by the port. It
is resolved when the code is generated, so it takes effect at every site that
can enter the routine, not only at runtime lookups. Every compiled caller of
the routine calls the replacement.

Use it once a routine is understood and a host implementation is clearer or
less constrained than the 65816 original. Two examples are a text engine that
reads a script of any length, and an OAM clip test that is widened for
widescreen. Do not use it to hide a recompiler bug. The plain ROM build stays
the faithful floor: `tools/v2_emit.py --no-hle` regenerates the program without
any replacement, for A/B validation (see [Validation](#validation)).

## Declaring a replacement

Add the directive to the bank cfg that owns the routine:

```
func TextStep 9000 exit_mx:1,1   # optional, but recommended
hle_func 9000 HleTextStep
```

- `<pc>` is the routine's 16-bit entry in this cfg's bank. The LoROM execution
  mirror (`$00`/`$80`) is covered automatically.
- `<c_function>` must be a valid C identifier. The port defines it, normally
  in `src/gen_stubs.c`, as `RecompReturn HleTextStep(CpuState *cpu)`.
- A `func` line is optional, but it gives the stubs a readable name
  (`TextStep_M1X1` rather than `bank_00_9000_M1X1`). It is also where an exit
  width other than the entry width is declared (see
  [Registers and flags](#registers-and-flags)).
- The PC must be the routine's real entry for the ROM revision being built.
  If it is wrong (in the middle of an instruction, or an address in a different
  revision), nothing is replaced. Callers keep running the compiled original,
  and the stub is reachable only through a runtime lookup of that address.
  Check that the generated callers name the stub.

`hle_spc_upload <pc> [live]` is a built-in special case of the same mechanism
for the standard SPC upload protocol. Its stub also performs the RTS itself.

## What the emitter produces

For the replaced PC (and its mirror), `recompiler/v2/emit_function.py` emits
one forwarding stub for each of the four M/X variants:

```c
RecompReturn TextStep_M1X1(CpuState *cpu) {
  ...                                       /* name/trace bookkeeping */
  RecompStackPush("TextStep_M1X1");
  cpu_take_tailcall_return_context(NULL, NULL);
  ...
  RecompReturn _r = HleTextStep(cpu);
  RecompStackPop();
  return _r;
}
```

All four variants are emitted once anything reaches the PC, and also when it is
declared but reached only at runtime (`program_emit.build_emission_entries`).
The dispatch table row lists all four, so the ROM bytes are never chosen
because the live M/X differs from what analysis saw.

The stub does **not** pop the return frame, charge cycles, check the LLE
scheduler deadline, or change any register. Those are the helper's
responsibility.

The PC is a hard decode boundary in both the native analyzer and the Python
emitter. Its ROM bytes are never compiled into another function's body, and
every edge onto it is a tail transfer to the stub. The analyzer still decodes
the original body as its own node, so any routines it calls are compiled and
available if the helper delegates to them.

## Which arrivals reach the helper

| Arrival | Reaches the helper | `host_return_valid` at entry |
|---|---|---|
| Compiled `JSR abs` | yes | 2 |
| Compiled `JSL long` | yes | 3 |
| Compiled `JSR (abs,X)` table, `ptrcall`, runtime-pointer `JSR` (`cpu_dispatch_call_pc`) | yes | 2, or the declared `frame:` |
| Compiled `JMP`/`JML`/`BRL`/branch, `ptrtail`, fall-through, a caller whose `end:` range covers the PC, `PEI`;`RTS` (`rtsstack`) | yes, as a tail transfer | the jumping function's own value |
| RTS/RTL trampoline or other dispatch-table lookup (`cpu_dispatch_pc_from`) | yes | 0 |
| Interpreter `JSR`/`JSL`/`JSR (abs,X)` (bounce to the dispatch row) | yes | 2 or 3 |
| Interpreter with bouncing off for the target (`SNESRECOMP_LLE_BOUNCE=0` under the LLE scheduler, `SNESRECOMP_LLE_INTERP_TARGET` or its target file, `interp_bridge_set_lle_bounce_exclusions`) | **no**, the ROM bytes run | n/a |
| Interpreter `JMP`/`JML`/branch/fall-through onto the PC | **no**, the ROM bytes run | n/a |
| Host glue calling the `void` alias (`TextStep(cpu)`) | yes | whatever the glue set |

The two interpreter limitations follow from the interpreter only intercepting
call instructions. Code that the analyzer leaves in LLE and that jumps into
the routine runs the original. If that matters, make the jumping code
AOT-eligible, or move the replacement to the routine the interpreter calls.

`tests/v2/test_hle_func_seam.py` pins every compiled arrival shape in the
table, with and without a `func` line.

## The helper contract

```c
RecompReturn HleTextStep(CpuState *cpu);
```

At entry, `cpu` holds the exact guest state at the routine's first
instruction: A/X/Y/S/D/DB/PB, P with its mirrors, `m_flag`/`x_flag`. The
helper must leave the state that the original's final instruction leaves.

### Return frame (Option-1 `cpu->S` ABI)

Generated callers push the real hardware return frame onto `cpu->S` before
entering the stub: 2 bytes for `JSR`, 3 for `JSL`, holding return address − 1
(`codegen._emit_return_frame_push`). Generated code then pops it in the
callee's own RTS/RTL (`codegen._emit_return`). The stub has no RTS, so:

**The helper must pop exactly what the replaced routine's terminating
instruction pops:** `cpu->S += 2` for an RTS routine, `cpu->S += 3` for an
RTL routine.

- Pop by the original's return opcode, not by `host_return_valid`. That value
  is 0 for dispatch entries, and for a tail arrival it describes the jumping
  function's frame. The frame on the stack is the one the original RTS/RTL
  would consume in every case.
- Leave the caller's `PB` alone. A generated `JSL` site saves and restores PB
  around the call.
- If the frame is left on the stack, the caller's later RTS finds it
  unbalanced. It pops the stale frame as a computed jump back into its own
  continuation, which on hardware runs that code twice. The end-to-end test
  shows this path being taken.
- A helper that never returns to its caller (a yield or a task switch) pops
  the frame when the guest's resume path would. See the scheduler yields in
  [`LLE_SCHEDULER.md`](LLE_SCHEDULER.md).

### Return values

| Return | Meaning | Stack obligation |
|---|---|---|
| `RECOMP_RETURN_NORMAL` | Return to the immediate caller. | Pop the routine's own frame. |
| `RECOMP_RETURN_SKIP_N` (N ≥ 1) | Return N guest call levels further out (the `PLA;PLA;RTS` idiom). Each generated `JSR`/`JSL` site returns `_r - 1`; tail sites pass it through unchanged. | Pop the frames that the hardware would have popped. |
| `interp_bridge_lle_yield_unwind(cpu, pc24)` | Hand control back to the LLE scheduler, which resumes interpreting at `pc24`. Only valid when `interp_bridge_in_lle_scheduler()` is true. | Leave S as the interpreted code expects at `pc24`. |
| Value returned by an engine tier call (`interp_tier_run_call_frame`, `cpu_dispatch_call_pc_pushed`, ...) | Pass it through unchanged. | The callee already handled it. |

A `void` host alias aborts if anything other than `NORMAL` reaches it.

A dispatch entry (`host_return_valid == 0`) is reached by an RTS/RTL
trampoline. In that case generated code would not host-return after popping.
It would dispatch on the popped PC: `cpu_dispatch_pc_from(cpu, pc24, S, site)`.
`NORMAL` after the pop is equivalent unless the popped PC is itself a function
entry (a `PEA target-1; JMP routine` chain). A helper that can be entered that
way must end the same way the generated RTS does.

### Registers and flags

- **Exit width.** The analyzer compiles each caller's continuation at the
  helper's declared exit width. By default that is the entry width (the
  analyzer seeds `exit == entry` for every HLE PC). If the original returns in
  another width, declare it: `func ... exit_mx:M,X`, `exit_mx_at`,
  `exit_mx_variant` or `exit_mx_set` (see
  [`EXIT_WIDTH_CONTRACTS.md`](EXIT_WIDTH_CONTRACTS.md)). Then make the helper
  leave exactly that width. A mismatch makes the continuation decode operands
  at the wrong width.
- **P and its mirrors.** Generated code reads the `_flag_*` mirrors and
  `m_flag`/`x_flag`, and also `P`. Keep them in agreement: set the mirrors and
  call `cpu_mirrors_to_p(cpu)`, or set `P` and call `cpu_p_to_mirrors(cpu)`.
  Any flag that the caller tests after the call (for example the carry from a
  final `CMP`) is part of the contract.
- **Register widths.** Use `cpu_write_a8/a16/a_m` and the X/Y helpers.
  Writing A in 8-bit mode preserves B. Index writes with `x_flag` set clear the
  high byte.
- **DB, D, A, X, Y, memory.** Leave them as the original leaves them on the
  path being replaced, including scratch RAM that callers read later.

### Cycles and timing

The caller has already charged the `JSR`/`JSL`/`JMP` instruction itself in its
block. The stub charges nothing. To keep frame timing identical, the helper
adds what the replaced instructions (including the final RTS/RTL, 6 cycles
each) would have charged on the path taken:

```c
cpu->cycles        += cpu_cycles;     /* bus cycles   */
cpu->master_cycles += master_cycles;  /* master clock */
```

`recompiler/snes_cycles.py` is the authority (`instr_cpu_cycles`,
`instr_master_cycles`, `region_speed`). `master_cycles` paces the APU, the LLE
scheduler's frame deadline, and coprocessors. Charging nothing is not a crash,
but the frame boundary moves and bit-exact comparison with the plain build is
lost. The stub also never checks `interp_bridge_lle_master_deadline_reached`.
A helper that runs for a long time inside the LLE scheduler should yield
through `interp_bridge_lle_yield_unwind` as generated blocks do. Any coprocessor
access needs `cpu->coprocessor_master_cycles` latched first.

### State

Keep all guest-visible state in guest RAM and `CpuState`, so that save states,
rewind and netplay digests capture it. Host-side state is acceptable only for
presentation that the guest never reads.

## Delegating to the original

A replacement covers all four variants, so no compiled original body exists to
call. There are two ways to run original behaviour from a helper:

- **The whole routine, exactly:** run the ROM bytes in the interpreter. Nested
  calls still bounce into compiled code. The interpreter consumes the caller's
  frame and charges its own cycles.

  ```c
  RecompReturn HleTextStep(CpuState *cpu) {
    prepare(cpu);                                        /* optional pre-work */
    RecompReturn r = interp_tier_run_call_frame(
        cpu, ((uint32)cpu->PB << 16) | 0x9000, 0x009000, 2 /* RTS frame */, NULL);
    if (r != RECOMP_RETURN_NORMAL) return r;             /* NLR / yield */
    finish(cpu);                                         /* optional post-work */
    return RECOMP_RETURN_NORMAL;
  }
  ```

  Each such run is recorded as a tier-2 `dispatch` discovery for the PC.
- **Other guest routines:** push the frame and use the paired dispatch. It
  runs an AOT body if one exists, and otherwise falls back to the interpreter:
  `cpu_push_jsr_return_frame(cpu); r = cpu_dispatch_call_pc_pushed(cpu,
  pc24, site, 2, NULL);` (or `cpu_push_jsl_return_frame` with 3). If `r` is not
  `NORMAL`, return `r - 1`, as a generated call site does. Never call a `void`
  alias from a helper: it pushes no frame and aborts on non-local returns.

Partial delegation, where the replacement runs into the middle of the
compiled original, is not supported. To get it, split the original with a
second `func` boundary at the resume PC and delegate to that entry.

## Example

The routine at `$00:9000` (entered M=1, X=1, ending in RTS) returns the larger
of A and `$0040` in A, and sets Z and N from the result:

```
# bank00.cfg
func MaxA 9000 exit_mx:1,1
hle_func 9000 HleMaxA
```

```c
/* gen_stubs.c */
#include "cpu_state.h"

RecompReturn HleMaxA(CpuState *cpu) {
  uint8 a = cpu_read_a8(cpu);
  uint8 m = cpu_read8(cpu, 0x00, (uint16)(cpu->D + 0x40));
  uint8 r = a > m ? a : m;
  cpu_write_a8(cpu, r);                 /* 8-bit A: B preserved */
  cpu->_flag_Z = r == 0;
  cpu->_flag_N = (r & 0x80) != 0;
  cpu_mirrors_to_p(cpu);
  cpu->cycles += 17;                    /* the replaced path, incl. RTS */
  cpu->master_cycles += 17 * 8;         /* SlowROM bank $00 */
  cpu->S = (uint16)(cpu->S + 2);        /* the original's RTS */
  return RECOMP_RETURN_NORMAL;
}
```

The cycle figures are illustrative. Take real ones from `snes_cycles.py` for
the instructions on the replaced path.

## Validation

1. Build once with replacements and once with `tools/v2_emit.py --no-hle`
   (every routine compiled from ROM). Compare guest RAM and cycle counters over
   the same input. Before the helper changes behaviour on purpose, it should
   match the plain build exactly.
2. Confirm that the stub is reached: generated callers name `<Name>_M?X?`, and
   the always-on dispatch log records runtime lookups of the PC.
3. `tests/v2/test_hle_func_seam.py` pins the emitter side: every compiled
   arrival shape, the analyzer boundary, all four stubs, the dispatch row, and
   (compiled and executed) the frame, `SKIP_N`, and tail-context behaviour of
   real generated callers.

## Pitfalls

- **No frame pop.** This is the most common mistake. The symptom is `cpu->S`
  drift, then stack corruption, or a continuation that runs twice.
- **Undeclared width change.** The original ends with `REP`/`SEP` but the cfg
  has no `exit_mx`.
- **Wrong PC for this ROM revision.** Nothing is replaced and the build stays
  quiet (see [Declaring a replacement](#declaring-a-replacement)).
- **Expecting interpreted jumps to reach the helper.** Only interpreted calls
  bounce.
- **Runtime-only overrides.** Swapping a function pointer at a
  `cpu_dispatch_*` lookup (the idea in issue #133) reaches only the
  sites that look the address up. Direct compiled calls and cfg-named dispatch
  targets bypass it. `hle_func` is resolved at generation time and does not
  have this gap.
