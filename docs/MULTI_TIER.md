# Native and interpreter execution

SNESRecomp combines generated native 65816 code with an interpreter fallback.
The ROM and live architectural state define correct behavior; a missing or
unproven native body must not silently execute a different width variant.
See [LLE-first analysis](LLE_FIRST_ANALYSIS.md) for static analysis and
[LLE scheduling](LLE_SCHEDULER.md) for runtime frame-driver integration.

## Generated call/return contract

Generated JSR/JSL calls maintain the guest return frame on `cpu->S` as well
as the host C call. `CpuState.host_return_valid` describes a proven paired
host caller and carries its guest frame size: 2 bytes for JSR, 3 for JSL,
and 0 when no paired caller is established. The old boolean value 1 is invalid.

Each generated function captures that value as `_hrv` and records its entry
stack pointer. RTS/RTL may return `RECOMP_RETURN_NORMAL` only with a proven
paired caller and balanced entry stack; otherwise they dispatch the popped
guest PC. Tail JMP/JML preserves the caller contract. Dynamic dispatch,
interrupt entry and interpreter transfers must establish the appropriate
paired or unpaired boundary instead of inventing a host return.

An unresolved transfer has no RTS/RTL opcode from which to infer a frame size.
`cpu_unresolved_abandon_balanced()` therefore uses the size the caller actually
pushed. Interpreter-to-native bounces use the paired-dispatch interface when
the host caller is known, preventing a guest return from running past its
matching frame or popping it twice.

The authoritative field/helper contracts are in `runner/src/cpu_state.h`,
`runner/src/cpu_state.c` and `runner/src/interp_bridge.c`; generated call and
return emission lives in `recompiler/v2/`. Regression fixtures cover stack
balance, width routing, continuations and interpreter/native transitions.

## Coverage and feedback

Fallback is a correctness floor, not proof that a game is fully recompiled.
Use the runtime's coverage diagnostics to distinguish native execution from
interpreter work and recover new static evidence through the supported tools.
Never claim native execution merely because an address has a registered body.

- [Coverage feedback](COVERAGE_FEEDBACK.md) documents discovery and regeneration.
- [Exit-width contracts](EXIT_WIDTH_CONTRACTS.md) describe safe width selection.
- [Disassembly authority](DISASSEMBLY_AUTHORITY.md) separates ROM facts from
  annotations and analysis guesses.
- [Differential co-simulation](COSIM.md) describes the internal comparison gates;
  [benchmarking](BENCHMARKING.md) covers reproducible measurement.

HLE/LLE replacement policy and performance-default promotion follow `AGENTS.md`.
Per-title hook addresses and gameplay adapters belong in each game's project.
Historical bring-up measurements remain under `docs/validation/`; current
issues and implementation tasks are tracked in central Beads.
