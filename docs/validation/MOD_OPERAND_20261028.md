# Mod-owned operands: engine qualification

The inherited ROM-mutable `live_operand` implementation has been replaced by
declared mod-owned immediates. Both CFG parsers reject `live_operand`. Program
emission registers supported sites in a generated constructor table, and the
runner includes the runtime in every build. Registration is sized from the
declarations, with no fixed active-site cap; unknown sites are refused.

Compiled and interpreted execution agree without changing cartridge bytes.
The interpreter's optional word-bus path, BIT immediate fetch, bank aliases,
register-width truncation, set/clear and duplicate registration are covered by
the execution tests. Unsupported declarations are refused even if the site
would execute only in the interpreter. Programs without the directive emit
no extra operand source or calls; removing the directive removes stale output.

Validation on 2026-10-28:

- Complete Python v2 suite: 615 passed. Focused post-review repeat: 39 passed.
- Rust analyzer library: 112 passed; binary tests: seven passed. Native release
  analyzer rebuilt and used by program-generation tests.
- ROM-free C suite: end-to-end exit 0 on Windows/MinGW and Ubuntu/WSL.
  The Linux run includes the POSIX account-secret fixture. Windows explicitly
  skips that POSIX fixture and its unsupported fiber snapshot backend; optional
  DSP-1 firmware checks remain skipped when private firmware is not supplied.
- New runtime compiles with `-Wall -Wextra -Werror` on MinGW.

The C runner also needed existing Windows test plumbing repaired: an updated
APU stub in the HDMA fixture, native environment calls, the network libraries
used by recomp-net, and SDL main handling. These changes do not alter runner
behavior. Linux's final affected tail was repeated successfully after those
test plumbing changes.

Logs, exact commands and exit-code JSON files are retained under
`G:/Temp/Codex/sm-qol-20261027/`: `engine-v2-tests-final.log`,
`operand-commit-tests.log`, `engine-rust-tests.log`,
`engine-c-gate-{windows,linux,linux-tail}.{log,json}`.

This qualifies the mechanism. Super Metroid's plugin/table conversion,
regeneration, stock trace, per-feature tier parity and owner playtest remain
separate acceptance work. No merge, push or release is implied.
