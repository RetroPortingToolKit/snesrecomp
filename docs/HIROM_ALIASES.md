# HiROM aliases and execution contexts

Standard HiROM maps the upper halves of banks 00/40/80/C0 to the same ROM
bytes. The same relationship repeats for the other bank indices. Lower
halves alias only between full ROM banks 40–7D and C0–FD; 7E/7F are WRAM.
FE/FF have no whole-bank XOR-80 execution mirror.

The Python emitter and native analyzer expand authored cfg contracts into
valid physical aliases before analysis. Local entries, hooks and declared
exit contracts take precedence over inherited ones. Unspecified aliases
inherit in sorted source-bank order. Friendly names keep their original
owner. RAM relocation and auto-vector configuration are not cloned.

Inherited contracts must actually apply at the alias: add an explicit local
entry/exit declaration when execution differs. Inferred exit widths and
compiled functions remain tied to their execution bank. For example, the
same `JMP $1000` bytes at 00:9000 and C0:9000 target RAM and ROM respectively.
The runtime may reuse an XOR-80 body within a low-bank pair or a full-bank
pair only after verifying the live cartridge ROM pointers. It never uses
that check to substitute a full-bank body for a low-bank body.

Inherited explicit indirect-dispatch targets follow the opcode's target width:
near JMP/JSR targets resolve in the executing bank, even when the cfg spells
the address in an authored physical alias. Long transfers retain their absolute
target banks. Null dispatch slots remain null. This distinction is enforced in
both decoders before native analysis or emission, so an alias cannot call a
full-bank body in place of its separately analyzed low-bank continuation.

Coverage reports include `rom_pc24` for physical grouping while retaining
the raw target, execution variant, outcomes and qualification provenance.
A successful C0 run cannot qualify bank 00. Coverage remains an analysis
seed and never overrides structural analysis or replay requirements.

Disassembly layout materialization and source export accept standard LoROM
and HiROM, including full-bank HiROM addresses below $8000. Authority ingestion
deduplicates identical physical instruction spans and detects conflicts or
code/data overlap across aliases. Entry widths retain their explicit HiROM
execution bank. The v1 authority schema describes one unconditional physical
instruction layout: alternate-width interpretations with different byte
boundaries need separate authority inputs, not a silently selected layout.

These changes do not generalize aliasing for SA-1, S-DD1 or other banked
mapping. Synthetic tests cover mapper boundaries, cfg precedence, short-jump
semantics, inherited near/long dispatches, native/emitter agreement, source
export and authority overlays. The reporter qualified corrected head `0dae9c8`
with Secret of Mana boot, field/flight, cutscene and shop replays: state, video
and master-clock checkpoints matched the hand-alias baseline in compiled and
interpreter-only runs. Matched warmed Release timings were within noise, with
no reported startup or working-set increase. See the
[replay and runtime report](https://github.com/RetroPortingToolKit/snesrecomp/pull/199#issuecomment-6400790503).
This evidence is title-specific and does not claim to fix SD3 issue #80.
