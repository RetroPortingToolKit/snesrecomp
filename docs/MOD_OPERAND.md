# Mod-owned immediate operands

Declare a supported immediate instruction in its bank configuration:

```
# bank03.cfg: CMP #limit at $03:8B27
mod_operand 8B27
```

The site is a hexadecimal 16-bit local instruction PC. Both parsers reject
duplicates, out-of-bank addresses and the removed `live_operand` directive.
Supported data instructions are `LDA LDX LDY ADC SBC AND ORA EOR CMP CPX CPY BIT`.
Status/control operands such as REP, SEP and MVN are refused: changing a mod's
value must not change instruction size or invalidate decoded M/X facts.

The emitter replaces the folded immediate with `snes_mod_operand(pc24, stock)`
and generates `mod_operands_v2.c`, which registers deduplicated physical ROM
offsets before main. Undeclared programs get no extra source file or operand
calls. Registration allocation failure stops startup rather than silently
leaving one tier unsupported. Registry storage is sized from declarations;
there is no fixed active-site limit or allocation during instruction execution.

Include `mod_operand.h` from the trusted plugin. `snes_mod_operand_set(pc24,
value)` sets a declared cartridge instruction's operand and returns false for
unknown or non-ROM addresses. `clear(pc24)` restores its stock operand;
`clear_all()` restores every site. `get(pc24, &value)` and `active_count()`
inspect active overrides. Reset callbacks must clear their feature's sites.

Values belong to the mod runtime. These APIs never write the cartridge image.
Aliases resolving to the same physical ROM offset share one value. AOT uses
the baked stock operand when inactive; the interpreter substitutes the same
bytes at the instruction's original M or X width, including BIT's separate
immediate fetch and optional word-bus callbacks. Opcode and cycle charges are
unchanged. An 8-bit operation uses the low byte of the supplied 16-bit value.

Both analyzers suppress value-derived constant-Z folds at a declared site,
retaining either reachable successor. Declared-site changes invalidate bank
emission caches. Data outside supported declared immediates belongs in typed
content loaders or declared `mod_hook` callbacks, not cartridge writes.

`tests/v2/test_mod_operand.py` executes generated and interpreted instructions
using the actual cart alias resolver, mixed M/X widths, set/clear, duplicate
registration and more than 1,024 declared sites, and verifies ROM immutability.
`test_program_emit.py` checks generated registration, alias deduplication,
deterministic repeated output and removal of stale generated tables.
