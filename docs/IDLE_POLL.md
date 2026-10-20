# Declared idle polls: replaying an interrupt wait

A whole-program LLE run (`interp_bridge_run_until_quiescent` under a master
deadline) interprets a vblank wait instruction by instruction until the
deadline, every frame, when the wait polls direct page:

```
LDA #$FF / STA $01      ; the NMI handler clears $01
LDA $01  / BNE -4       ; interpreted to the deadline
```

The quiescent detector treats every read below `$2000` as live, because
decompressors poll direct page too. So it never recognises this loop as a
wait. Letting it yield instead would end the window at a different clock than
spinning, which in a frame that only just fits decides a lag frame.

`interp_bridge_set_idle_poll` lets the host say which instruction is such a
poll. The bridge then **replays** the wait's cycle to the deadline instead of
interpreting it, with the same result as interpreting it.

## API and lifetime

```c
bool interp_bridge_set_idle_poll(uint32_t pc24);
void interp_bridge_clear_idle_polls(void);
```

- Up to 8 PCs. LoROM FastROM bit 7 is masked, so `$00:8904` and `$80:8904`
  are the same declaration. Declaring a PC twice is accepted. A ninth PC
  returns `false` and is not declared.
- A declaration is host configuration, not guest state. It lasts until
  `interp_bridge_clear_idle_polls()`: across runs, frames, resets and
  savestate loads. It is not saved in a savestate. A host declares after
  loading its ROM and clears when it loads a different one.
- Declaring a PC is a promise that what the instruction reads changes only
  when an interrupt handler runs. The guards below catch the cases the bridge
  can see. A promise that is wrong in a way it cannot see (memory that some
  other path writes during the wait) is the host's error.

## When a cycle is replayed

The detector records the architectural state before every interpreted step.
A cycle is replayed when, under a master deadline, all of the following hold:

1. **Stable state.** The same registers, flags, widths and PC recur, at least
   twice, within 32 steps.
2. **No write** in between (write epoch unchanged).
3. **No volatile read.** Every read was WRAM or ROM: no MMIO, APU port,
   coprocessor window, cartridge RAM or open bus (volatile epoch unchanged).
4. **No live read but the declared poll's.** A direct-page read from any
   other instruction is still live.
5. **The cycle contains a declared PC.**
6. **No IRQ can be taken.** Either I is set at every step in the recorded
   cycle, or neither the H nor the V timer is armed. Equal endpoint flags
   alone do not establish this: a cycle can contain both `SEI` and `CLI`.
7. **No self-running coprocessor:** Super FX, Cx4 and SA-1 carts are never
   replayed.
8. **No HDMA into the WRAM data port.** No active HDMA channel targets
   `$2180-$2183`. That is the one device that can change the polled byte
   without the CPU.

If any guard fails, nothing changes: the run continues exactly as if nothing
were declared. The quiescent detector keeps its own read epoch, which still
counts the declared poll's reads, so a declaration never changes what that
detector decides.

## What the replay reproduces

The cycle's steps recur with the costs they were just measured at. For each
replayed step, in order:

- the step cap and then the master deadline are checked, as the interpreted
  loop checks them before each step;
- the CPU cycles and master clocks of that step are charged;
- the DRAM refresh tax is charged;
- open bus becomes that step's value;
- the APU's pending time grows by that step's master clocks and is flushed at
  the same threshold. On a relative APU timeline the beam is synced first, as
  the interpreted step would have.

The bridge then leaves exactly where the interpreted loop would:

- **Deadline:** resume site `DEADLINE`, at the PC and registers of the step
  the deadline check stopped.
- **Step cap reached first:** through the ordinary step-cap exit, with that
  step's state.

The beam and coprocessors are synced to the final clock, the CPU state is
written back and the APU flushed.

## Deadline and event boundaries

Under a master deadline the bridge samples one kind of event between
instructions: an asserted IRQ, which ends the run with resume site
`IRQ_PENDING`. NMI is never delivered inside a run. The frame driver sets the
deadline at the next event it owns (the vblank NMI, a scheduler tick) and
delivers it after the run returns. A replay therefore never crosses an event
boundary:

- It never runs past the deadline.
- It is refused whenever an IRQ could assert and be taken during the wait.
- An IRQ that is already pending with I clear ends the run at the loop head,
  before the detector runs.

The interpreted loop syncs the beam after every step; the replay syncs it at
APU flushes and at the end. The CPU cannot observe the difference: the
replayed cycle reads no MMIO (guard 3), no IRQ can be taken (guard 6), and
HDMA cannot write the polled memory (guard 8).

## Tests

`tests/interp816/bridge_test.c`, scenario S18, runs each case twice, spinning
and declared. It compares rc, resume site and PC, master clock, cycle count,
A/X/Y/S/P, open bus, APU flush count and a hash of low RAM and WRAM:

| Case | Expected |
|---|---|
| `LDA $01 / BNE` wait, I set | identical, under a tenth of the steps interpreted |
| same, declared through the `$80` mirror | identical, replayed |
| declaration kept across runs, then cleared | replayed, then spinning again |
| table: 8 slots, a repeat accepted, the ninth refused, clear frees it | |
| V-IRQ or H-IRQ armed, I clear | identical, every step interpreted |
| H-IRQ armed, I set | identical, replayed |
| H-IRQ armed, I changes within the cycle | identical IRQ exit, interpreted |
| IRQ pending, I clear | both leave after one step with `IRQ_PENDING` |
| the declared poll reads an APU port | identical, every step interpreted |
| a `$4211` read elsewhere in the cycle | identical, every step interpreted |
| HDMA into `$2180` / into `$2118` | interpreted / replayed, identical |
| step cap before the deadline | both leave through the step-cap exit, identical |

Removing any one of the guards for volatile reads, IRQ, HDMA or the step cap
makes its scenario fail.
