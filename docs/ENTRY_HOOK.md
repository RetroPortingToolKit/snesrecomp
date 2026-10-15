# Observing a compiled instruction: `entry_hook`

`entry_hook` lets a host watch a routine run without keeping it interpreted.
Typical use: a widescreen renderer records every call of a sprite-list routine
(arguments in A/X/Y, return address on the stack) to redraw objects the game
culls at the original view's edge. The interpreter offers this through
`interp_bridge_add_pre_opcode_hook`; compiled callers call a compiled body
directly and never pass through that hook. `entry_hook` is the compiled
counterpart.

## Declaring a hook

```
# bank00.cfg
func DrawSpriteList 9437 end:94c0 entry_mx:1,1
entry_hook 9437 HostSawSpriteList
```

The port provides the function:

```c
void HostSawSpriteList(const CpuState *cpu, uint32 pc24);
```

`pc24` is the 24-bit PC of the instruction about to run, including the bank
the code was entered through (`$00:9437` or the LoROM mirror `$80:9437`).

One hook per PC. The configuration is rejected for a duplicate PC, a name that
is not a C identifier, or a PC whose routine is replaced by `hle_func` or
`hle_spc_upload` (no compiled instruction runs there: observe the call in the
helper instead).

## The callback is observational

The callback may read anything: registers, flags, widths, the stack and guest
memory through `cpu->ram`. It must not change CPU state, guest memory, MMIO or
the clocks, and must not call bus accessors (`cpu_read8` and friends can have
side effects on MMIO, and they take a non-const `CpuState *`).

The compiled body continues with the facts the analyzer proved for this point
(M/X widths, stack shape, constant folds), so a hook that changed state would
run the rest of the block on facts that no longer hold. This is the difference
from the interpreter's pre-opcode hook, which may edit `CpuState` and redirect
the PC: anything that changes behaviour belongs in `hle_func` or in the
interpreter hook on an LLE-only routine.

## When it fires: once per execution of the instruction

Compiled code calls the hook each time it is about to execute the instruction
at the hooked PC. That is exactly when the interpreter's pre-opcode hook at
the same PC fires, so a host that registers the same observer for both tiers
sees one report per execution, whichever tier executes it:

```c
static void interp_saw(CpuState *cpu, uint32_t pc24) {
  HostSawSpriteList(cpu, pc24);
}
/* at startup */
interp_bridge_add_pre_opcode_hook(0x009437, interp_saw);
```

The cases that cross tiers or repeat:

| Case | Compiled report | Interpreter report |
|---|---|---|
| Call into the compiled body | once, before the first instruction | none |
| Deadline expires in the prologue or at the hooked block | none: the yield happens first | once: the interpreter resumes at the hooked PC and executes it |
| Branch back to the hooked instruction, compiled | once per pass | none |
| Branch back that expires the deadline at the hooked block | the passes executed compiled | the pass it resumes |
| Recursive or repeated call | once per execution, in each invocation | none |
| Another compiled body reaches the PC (fall-through, `end:` range covering it) | once, in that body | none |
| An interpreted JSR/JSL into a compiled variant | once: the interpreter bounces into the body | none |
| A variant with no compiled body (LLE-only) | none | once per execution |

A branch back to the hooked instruction is an execution and is reported. This
matches the interpreter, and that agreement is what keeps the stream
independent of where a deadline happens to land. A routine whose entry is
also a loop head therefore reports each pass. A host that wants calls only can
tell them apart by the return frame (`cpu->S` and the bytes above it).

## Placement in generated code

Every hooked PC starts a block wherever it is decoded, including mid-run in
another body that falls into it. In that block the call sits after the
deadline check and before the block's static cycle charge:

```c
  L_9437_M1X1:
    cpu_trace_block(cpu, 0x009437);
    WatchdogCheck();
    if (interp_bridge_lle_master_deadline_reached(cpu)) {
      RecompStackPopYield();
      return interp_bridge_lle_yield_unwind(cpu, ...0x9437u);
    }
    HostSawSpriteList(cpu, 0x009437u);
    cpu->coprocessor_master_cycles = cpu->master_cycles;
    cpu->cycles += ...;
```

Nothing of the instruction has run or been charged when the hook sees the
state. The extra block boundary only adds one deadline check.

## Analysis

The directive is emission-only. It changes no decode width, control-flow edge
or analysis fact, so the native analyzer ignores it and the manifest is the
same with or without hooks.

## Tests

`tests/v2/test_entry_hook.py` compiles hooked routines and runs them with a
recording observer and a scripted deadline. It covers a loop back to the
hooked entry, a deadline in the prologue, at the hooked block and on the
back-edge, two calls, recursion, and a fall-through from another body. A
program-level emission checks the routine's own body, a covering body and the
`$80` mirror body.
