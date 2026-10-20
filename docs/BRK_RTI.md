# A BRK that only returns: `brk_rti`

Some programs contain a `BRK` whose native vector points at a bare `RTI`. ROM
hacks do this when they pad their own code with `00 00`. On hardware such a
BRK is a round trip and nothing else:

1. `BRK` (native mode) fetches its signature byte, pushes PB, the address
   past the signature byte and P, sets I, clears D and jumps through
   `$00:FFE6`.
2. The `RTI` there pulls P, the address and PB.
3. Execution continues at the BRK's address + 2 with P, PB and S as they
   were.

What remains are the four bytes written below S, and the time.

Without a declaration every BRK ends decode and the native analyzer marks
the routine `brk_at_<site>` structural poison. That is right for the common
case: zero-filled data decodes as `BRK #$00`, and decode must keep stopping
there. `brk_rti` declares the one site where the round trip is meant.

## Declaring a site

```
# bank00.cfg
brk_rti 8002
```

One line per site, a 16-bit PC in the cfg's bank (both LoROM mirrors are
covered). A missing argument or a duplicate site is a configuration error in
both the Python and the Rust parser.

## Validation

The emitter (`recompiler/v2/brk_rti.py`) and the native analyzer
(`check_brk_rti_site` in `recompiler-rs/src/bin/analyze.rs`) apply the same
rule, with the same wording, to every declared site, whether or not any
routine decodes it:

- the ROM byte at the site is `$00` (BRK): otherwise
  `brk_rti at $XXXXXX: not a BRK in ROM`;
- the native BRK vector at `$00:FFE6` points at a `$40` (RTI) in ROM:
  otherwise `brk_rti at $XXXXXX: the native BRK vector $XXXX is not a bare
  RTI`.

A site one side accepts is therefore a site the other accepts.

## What is compiled

Decode falls through the site with the widths and the PHP stack unchanged.
The analyzer does not count it as poison. The emitter compiles the round
trip in place:

```c
/* BRK to a bare RTI (brk_rti): the round trip, then on */
if (cpu->emulation) {
  return interp_tier_dispatch_tail(cpu, ..., 0x008002u, _entry_s, _hrv);
}
{ uint16 _s = cpu->S;
  cpu_mirrors_to_p(cpu);
  cpu_write8(cpu, 0x00, _s, cpu->PB);
  cpu_write8(cpu, 0x00, (uint16)(_s - 1), 0x80);
  cpu_write8(cpu, 0x00, (uint16)(_s - 2), 0x04);
  cpu_write8(cpu, 0x00, (uint16)(_s - 3), (uint8)cpu->P);
  cpu->cycles += 7;
  cpu->master_cycles += 7 * 8; }
goto L_8004_M1X1; /* implicit fall-through */
```

- **Stack bytes:** PB at S, the continuation address high and low at S-1 and
  S-2, and P at S-3. S itself ends unchanged.
- **P and PB:** unchanged. The I set and D cleared by the BRK are undone by
  the RTI's pull.
- **Cycles:** the BRK's 8 (7 + 1 native) are part of the block's static
  charge. The RTI's 7 (6 + 1 native) are charged here, at the speed of bank
  `$00`, where the vector points.
- **Emulation mode:** BRK goes through `$FFFE`, the IRQ vector, which the
  directive did not validate. The compiled code hands the BRK, with nothing
  pushed, to the interpreter, which executes it architecturally.

Undeclared BRKs keep their ordinary behaviour: they end decode and stay
structural poison.

## Interrupts

On hardware an NMI can be taken between the BRK and the RTI (the BRK has
set I, so an IRQ cannot). Compiled code services interrupts at block
boundaries, so a pending NMI is taken at the next deadline check instead.
This is the same granularity as any other compiled instruction sequence. The
NMI handler then sees the BRK frame already pulled; it is still in memory
below S.

## Tests

`tests/v2/test_brk_rti.py`:

- parsing and parse errors;
- emission: fall-through, the emulation guard, no compiled round trip
  without the directive;
- rejection of a non-RTI vector and a non-BRK site;
- **parity:** for a wrong vector, a non-BRK site and two sites no routine
  decodes, Python and the native analyzer give the same error;
- **manifest:** the declared site removes the `brk_at_` poison and the code
  after it is analysed with a proven exit;
- **compiled versus interpreted:** the same `BRK / STZ / RTS` runs compiled
  and in `interp816` with architectural BRK vectoring, in three width and
  flag states. It compares P, PB and S, the four stack bytes, the
  continuation (the STZ's effect) and the total cycles, and checks the
  interpreter's PC, S and flags right after the BRK and after the RTI. In
  emulation mode the compiled code exits to the interpreter at the BRK
  without writing the stack.

The round trip found that `interp816` charged a native BRK 7 cycles; it now
charges 8, as it already did for COP and RTI.
