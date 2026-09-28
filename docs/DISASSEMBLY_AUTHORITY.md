# Disassembly authority and execution validation

A disassembly can establish ROM instruction boundaries and code/data separation.
It cannot, by itself, establish that generated C preserves CPU state, host hooks,
interrupt scheduling, or audio timing. Validate those layers separately.

## Reference inputs used on 2026-09-28

| Title | Reference source | Revision | Assembler | Headerless ROM SHA-256 |
|---|---|---|---|---|
| Super Mario World (USA) | [SMWDisX](https://github.com/IsoFrieze/SMWDisX) | `30643c7` | Asar 1.91, `--define _VER=1` | `0838e531fe22c077528febe14cb3ff7c492f1f5fa8de354192bdff7137c27f5b` |
| Super Metroid (Japan, USA) | [sm_disassembly](https://github.com/InsaneFirebat/sm_disassembly) | `11c906f547edc1b57f5a5923cf977fe7b50a3694` | Asar 1.91 | `12b77c4bc9c1832cee8881244659065ee1d84c70c3d29e6eaf92e6798cc2ca72` |
| A Link to the Past (USA) | [usdasm](https://github.com/spannerisms/usdasm) | `bcdd96e96ee092824078754b6192a5b40f1f39ac` | Futaba 0.2.1-beta | `66871d66be19ad2c34c927d6b14cd8eb6fc3181965b6e517cb361f7316009cfb` |

All three assembled images match the complete local ROM byte for byte. Asar is
pinned at `6a6dbda` (1.91). Its WLA address-to-line output includes data spans that
the 1.81 build bundled with the Super Metroid reference omits. Use 1.91 for both
the assembled image and symbols. An empty SMW output receives a 512-byte copier
header; the exporter accepts that header only when the remaining image matches.

Extract reference assets from the locally owned ROM. Super Metroid provides
`tools/rip_assets.py`. US ALttP's `incbin` ranges are described by the source's
explicit address markers and size comments. Its assembly uses `futaba build
alttp.futaba`; the tested release requires .NET 10. Futaba's interactive final
key wait can throw with redirected stdin after successful assembly. Check the
assembly diagnostics and complete image equality; never treat that exception
alone as an assembly-success signal.

## Reproducible pipeline

Keep reference sources, ROMs, extracted assets, generated authority, and C output
outside tracked source. The JSON authority contains instruction bytes.

1. Assemble the pinned reference into a new image with address-to-source symbols.
2. Export its authority using the reviewed SMW source/macro classifier:

   ```text
   python tools/export_disassembly_authority.py --format asar65816
     --source <sm-disassembly/src> --source-commit <pinned-commit>
     --rom <owned-rom> --assembled-rom <assembled.sfc>
     --assembler <asar.exe> --symbols <assembled.sym>
     --classifier <SuperMarioWorldRecomp/tools/ingest_smwdisx.py>
     --out <new-authority.json>
   ```

   Use `smwdisx` for SMW and `usdasm` for US ALttP. The latter uses explicit
   source address annotations and does not take `--symbols`. The exporter
   rejects a different assembled image, records input hashes, and reports
   unknown source spans. Lengths come from assembly/source boundaries, not the
   decoder being tested. Mixed or unclassified macro bodies remain unknown.

3. Create an isolated configuration overlay:

   ```text
   python tools/ingest_disassembly_authority.py --rom <owned-rom>
     --authority <authority.json> --cfg-dir <title/recomp>
     --out-dir <new-overlay-directory>
   ```

   Byte and operand boundaries constrain both analyzers. `authority_data` is a
   hard code exclusion; ordinary `data_region` remains a discovery hint. Proven
   ROM mirrors share constraints. Evidence attaches to an existing mirror's cfg
   when appropriate, preserving HLE hooks and function boundaries. SA-1 windows
   are not treated as ordinary LoROM mirrors. Runtime program-bank state is
   never normalized by the importer.

4. Generate and audit the actual emitted decode:

   ```text
   python tools/audit_disassembly.py --rom <owned-rom>
     --authority <authority.json> --cfg-dir <overlay-directory>
     --source-root <title/src> --out-dir <new-generated-directory>
     --report <audit.json> --disassembly-entry-modes
   ```

   The optional last flag probes all four M/X combinations at existing entries
   with authoritative instruction boundaries. It does not declare all four
   valid: byte constraints, structural analysis, and exit proofs still decide.
   Audit observes the decoder invoked by the emitter after native analysis, not
   a retired analysis pipeline. It reports data execution, operand entries,
   length conflicts, unknowns, unique matched ROM instructions, and bodies not
   observed (including HLE). It refuses reused output or a changing authority.

5. Build with the same runtime for the previous-coverage control and expanded
   candidate. `SM_GEN_DIR`, `SMW_GEN_DIR`, and `ZELDA_GEN_DIR` allow isolated C
   trees. Apply each title's normal generation hooks. Run identical continuous
   routes with copied SRAM through `tools/coverage_replay.py`; include guest
   state, presented pixels, video resources, and any relevant audio evidence.
   A finite normal exit flushes checkpoint costs. Never pause or step either
   runtime to obtain comparison evidence.

## Findings that distinguish failure classes

- **Discovery:** wrong-width instructions, operand entries, and data-as-code are
  rejected by byte authority. A RAM capture appended to the analysis buffer must
  not make an out-of-image cartridge bank appear materialized.
- **Discovery reproducibility:** a renamed generated tree remains generated.
  Host-root scanning excludes trees with `program_manifest.json`, including
  archived output beneath `src`, so old generated prototypes cannot seed roots.
- **Analysis:** the exit-mode solver must compare immutable previous-round
  facts. Removing facts from that previous set while propagating mirrors caused
  a real Super Metroid non-convergence loop.
- **Emission/linkage:** proving a body at a mirror does not prove that the raw
  target's C symbol exists. Direct tail routing needs exact ownership; dispatch
  switches resolve their body owner separately.
- **Host contracts:** adding an authority-only mirror cfg must not shadow the
  original cfg's audio uploader or scheduler hooks. This caused Super Metroid's
  first observed expanded-build mismatch despite valid instruction boundaries.
- **AOT semantics:** JSL must push the live program bank, including when a body
  executes through a mirror. A generated body's address is not that register.
  See the [WDC W65C816S datasheet](https://www.westerndesigncenter.com/wdc/documentation/w65c816s.pdf),
  table 5-7, addressing mode 4c (page 37). A compiled emitted-frame regression
  exercises both `$0B` and `$8B` execution of the same body.
- **Short-transfer semantics:** JSR and short pointer calls retain the live
  program bank. Recovered tables must not use a JSL bank-changing envelope;
  interpreter fallback targets must combine the live bank with the short
  address. Executed C regressions cover direct, indexed-table and pointer-table
  calls in both mirrors, with both AOT and missing-variant fallback.
- **Continuations and clocks:** entry/block deadlines, memory-poll yields,
  WAI, block-move interruption, and unresolved continuations retain the live
  bank too. Code-region cycle weighting uses live PBR and MEMSEL: a generated
  low-bank body may be executing in FastROM. This corrects bank selection
  within the existing aggregate cycle model; it does not establish bus-cycle
  accurate timing for every emitted instruction.
- **Existing scheduler failure:** Super Metroid's synchronous message-box
  counter wait could be reached inside a nested interpreter fallback. Only the
  outer scheduler recognized this secondary wait, so both previous-root and
  expanded builds exhausted the interpreter and froze the guest while the host
  kept presenting frames. The bridge now hands these waits outward with the
  guest registers and stack intact. A synthetic nested counter-wait regression
  checks 8/16-bit memory reads, retained PHP/JSL frames, and resumed PLP/RTL.
  Replay validation rejects interpreter caps and scheduler bails even when the
  process exits successfully and its frozen frame dumps match.
- **Capture identity:** hash the headerless source image, not the mirrored bus
  allocation. Super Metroid's 3 MiB image is expanded to 4 MiB for mapping;
  hashing the latter made otherwise readable captures fail correct-ROM ingest.
- **Reference validity:** denying every AOT entry in a host-driven port can
  bypass required scheduling hooks and break boot. Such a run is not a valid
  interpreter oracle. Keep its failure evidence, then use a matched AOT control
  and independent instruction/disassembly checks to identify the cause.

Matching finite routes is evidence for those routes. A control can contain a
pre-existing bug, and shared emulation code is not an independent hardware
oracle. Keep mismatches and unqualified reductions visible; manual gameplay
review remains required before integrating these candidates.
