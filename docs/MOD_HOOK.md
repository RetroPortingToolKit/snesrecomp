# Changing guest behaviour at a PC: `mod_hook`

`mod_hook` runs a host function immediately before a guest instruction, in
compiled and interpreted code alike, and lets that function change the CPU
state and guest memory or send execution to another instruction of the same
routine. It is switched on and off at run time, so a gameplay mod (Super
Metroid respin, shinespark steering, instant bomb, ...) can arm its hooks when
its feature is enabled and disarm them when it is not, with the rest of the
routine still compiled.

Related tools, and when not to use them:

| Need | Use |
|---|---|
| Watch a routine without changing it | [`entry_hook`](ENTRY_HOOK.md) |
| Replace a whole routine with C, always | [`hle_func`](HLE_FUNC.md) |
| Give a declared immediate a mod-owned value | [`mod_operand`](MOD_OPERAND.md) |
| Change behaviour at one PC, switchable at run time | `mod_hook` |

## Declaring a hook

```
# bank90.cfg
mod_hook        A3F2 SmQolRespin
mod_hook_target A3F2 A41C      # a PC SmQolRespin may send execution to
mod_hook_target A3F2 A47B      # the routine's RTS: return early
```

Both PCs are hexadecimal **16-bit local addresses** in the cfg's bank, like
`entry_hook`. A target always belongs to one hook: `mod_hook_target <hook>
<target>`. The emitter applies a bank's hooks to every execution bank that
aliases the same ROM byte (the LoROM `$80` mirror; HiROM's `$00/$40/$80/$C0`
views), and all aliases share one armed flag.

The configuration is rejected, by both the Python cfg loader and the native
analyzer, for: a missing argument, a name that is not a C identifier, a PC
above `$FFFF`, a duplicate hook PC, a duplicate target, a target whose hook is
not declared, and a hook or target inside a routine replaced by `hle_func` or
`hle_spc_upload` (its entry PC, or any PC inside that routine's `end:` range).
Emission additionally rejects a target that a body compiling the hook does not
compile at the hook's M/X widths, and a target at a different stack depth
than the hook.

## The callback

```c
#include "mod_hook.h"

uint32 SmQolRespin(CpuState *cpu, uint32 pc24) {
  if (!(cpu->ram[0x8B] & 0x80)) return 0;          /* no jump held: continue */
  cpu->A = 0x0012;                                 /* change registers ...   */
  cpu->ram[0x0A1C] = 0x81;                         /* ... and memory         */
  return (pc24 & 0xFF0000u) | 0xA41Cu;             /* continue at $A41C      */
}
```

`pc24` is the PC about to execute, including the bank it was entered through.

The callback **may** change `A`, `X`, `Y`, `D`, `DB`, the flag mirrors
`cpu->_flag_N/V/Z/C/I/D`, and guest memory (`cpu->ram`, or the bus helpers).
With `x = 1`, the high bytes of `X` and `Y` are cleared afterwards, as on
hardware.

The callback **must not** change `M`, `X` or `E` (`cpu->m_flag`,
`cpu->x_flag`, `cpu->emulation`), `S`, or `PB`. The compiled code after the
hook was generated for the hook's widths and stack depth. A redirect target
must be at the same stack depth too. The stack bytes holding pushed registers
and return frames belong to the routine; leave them alone.

The return value is `0` to execute the hooked instruction, or a declared
target to continue there instead. The target may be given in any bank that
aliases the hook's bank. The runtime moves it to the executing bank. A
return-style hook redirects to one of the routine's own `RTS`/`RTL`/`RTI`
instructions.

### Checks

| Violation | Dev build (`SNESRECOMP_MOD_HOOK_STRICT`, default unless `NDEBUG`) | Production build |
|---|---|---|
| M, X or E changed | abort, naming the hook | restored, logged once per hook |
| S or PB changed | abort | restored, logged once per hook |
| Undeclared non-zero return | abort | treated as `0`, logged once per hook |

## Arming

`runner/src/mod_hook.h`:

```c
bool snes_mod_hook_arm(uint32 pc24, bool armed);  /* false: no hook declared there */
void snes_mod_hook_disarm_all(void);
bool snes_mod_hook_is_armed(uint32 pc24);
unsigned snes_mod_hook_count(void);
const SnesModHookDecl *snes_mod_hook_at(unsigned index);
```

Every hook starts disarmed. `pc24` may be any alias (`$00:A3F2` and
`$80:A3F2` arm the same hook). Arm during a mod plugin's activation and
disarm it in its reset callback (docs/MOD_AUTHORING.md). A port linking
several program modules arms the PC in every module that declares it.

Arming also registers the interpreter pre-opcode hook at every alias of the
PC (`interp_bridge_add_pre_opcode_hook`). Disarming removes exactly that
registration with `interp_bridge_remove_pre_opcode_hook`; other owners'
hooks at the same PC stay. A hook may disarm itself or another hook from its
callback.

## Placement in generated code

Each hook and target PC starts a block wherever it is decoded, in the
routine's own body, in any body that falls into it, in every M/X variant and
in mirror-bank bodies. In the hooked block, the test sits after the deadline
check and before the block's static cycle charge. Disarmed, it costs one load
and one branch:

```c
  L_A3F2_M0X0:
    cpu_trace_block(cpu, 0x90A3F2);
    WatchdogCheck();
    if (interp_bridge_lle_master_deadline_reached(cpu)) {
      RecompStackPopYield();
      return interp_bridge_lle_yield_unwind(cpu, ...0xA3F2u);
    }
    if (g_snes_mod_hook_armed[3]) {
      uint32 _mh = snes_mod_hook_dispatch(cpu, 3u, 0x90A3F2u);
      if (_mh == 0x90A41Cu) goto L_A41C_M0X0;
      if (_mh == 0x90A47Bu) goto L_A47B_M0X0;
    }
    cpu->coprocessor_master_cycles = cpu->master_cycles;
    cpu->cycles += ...;
```

Nothing of the instruction has run or been charged when the callback runs.
The emitted operations read registers and flags from `CpuState`, so the
callback's changes are what the instruction sees. A redirect is a `goto` to
the target's block at the hook's widths. That block exists because the
target is a block leader and emission verified it is compiled there.

When an `entry_hook` shares the PC, the `mod_hook` runs first. A redirect
skips the observer, because the hooked instruction then does not execute.

The program's table is emitted as `mod_hooks_v2.c`, only when the program
declares a hook. It contains, for each hook: its slot, its aliasing PCs, its
targets, and the armed flags (`g_snes_mod_hook_armed`). A constructor
registers it before `main()`. A program without the directive generates
exactly what it did before the directive existed. With `--module-prefix`,
the table's symbols are namespaced like the rest of the module.

The runner must compile `runner/src/mod_hook.c`.

## When it fires: tiers

Armed, the callback runs once per execution of the hooked instruction,
whichever tier executes it. Both tiers apply the same checks, the same target
normalisation and the same redirect.

| Case | Compiled | Interpreter |
|---|---|---|
| Call into the compiled body | once, before the instruction | none |
| Deadline expires in the prologue or at the hooked block | none: the yield happens first | once: the interpreter resumes at the hooked PC |
| Branch back to the hooked instruction, compiled | once per pass | none |
| Branch back that expires the deadline at the hooked block | the passes executed compiled | the pass it resumes |
| Recursive or repeated call | once per execution, in each invocation | none |
| Another body reaches the PC (fall-through, `end:` range covering it) | once, in that body | none |
| Mirror-bank body (`$80:xxxx`) | once, with the mirror `pc24` | none |
| A variant with no compiled body (LLE-only) | none | once per execution |
| Redirect | `goto` the target block | `interp_bridge_pre_opcode_redirect` |
| Disarmed | one load + branch, no call | no registration |

## Analysis

The callback can change any value the analyzers might otherwise have proven,
so **no value fact crosses a hook or a target**. A redirect target is a join
the decode graph does not show. The Python decoder and the native analyzer
apply the same barrier:

| Fact | Treatment |
|---|---|
| Constant-Z branch fold (`LDA #0 ; BEQ`) | not applied when the branch is a hook or target |
| Guarded pointer recovery (`... STA dp ; JMP (dp)`) | the backward chain stops at a hook PC (nothing earlier donates a value) and fails at a target PC (a join) |
| Duff stride-runway recovery | not attempted when a hook or target lies inside the pattern |
| TSC/TCS entry-stack restore | not proven in a function that decodes a hook or target |
| Inline-argument and JSL dispatch-helper idioms | not recognised when the scan meets a hook or target past the routine's entry |
| M/X widths, stack depth, return frames | facts **by contract** (checked at run time, see above) |

The decode cache key includes both PC sets. The manifest stays
deterministic, and without the directive it is unchanged. A hook that makes an
indirect jump unresolvable leaves it unresolved. Generation then reports it
like any other unresolved site. Move the hook, or declare the jump with
`indirect_dispatch`.

## Tests

`tests/v2/test_mod_hook.py`:

* cfg errors in both parsers;
* the barrier in the Python decoder, and Python and native analyzer
  manifests agreeing (and repeatable) with the directive present;
* compiled hooked routines executed in a C harness:
  * disarmed equals no directive;
  * armed continue with changed registers and memory;
  * redirect to a mid-routine target and to an `RTS`;
  * the `$80` mirror body;
  * a deadline in the prologue, at the hooked block and on a loop back-edge;
  * recursion;
  * fall-through from another body;
  * the `LDA #0 ; BEQ` fold that would be wrong without the barrier;
* the same scenarios through `interp_bridge` with the real interpreter,
  reaching identical final states;
* dev-build traps and production corrections;
* program emission into every body, the mirror and the table, and no change
  without the directive.

`recompiler-rs` unit tests cover the native parser and barriers.
