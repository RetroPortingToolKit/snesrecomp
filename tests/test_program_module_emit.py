"""Program-module emission: descriptor, symbol namespace, identity rules.

One generated tree is one program module (docs/CONTENT_VARIANTS.md). These
tests pin the three generated artifacts that let two modules share one link:
  - module_v2.c registers the tree's tables under a stable id,
  - module_namespace.h renames every generated definition AND call, and is
    derived from the emitted text rather than a post-hoc regex,
  - the identity rules refuse ids/prefixes that cannot be C identifiers.
"""
import pathlib
import re
import sys

TESTS_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
sys.path.insert(0, str(REPO_ROOT / 'recompiler'))

from v2.program_analysis import ProgramManifest  # noqa: E402
from v2.program_emit import (  # noqa: E402
    collect_bank_symbols,
    emit_dispatch_table,
    emit_module_descriptor,
    emit_module_namespace,
    validate_module_identity,
    MODULE_NAMESPACE_HEADER,
)

_SAMPLE_BANK = """\
/* header */
#include "funcs.h"

RecompReturn GameMode08_FileSelect_M1X1(CpuState *cpu) {
  return GameMode08_FileSelect_Entry3_M1X1(cpu);
}
RecompReturn bank_00_9CB0_M1X0(CpuState *cpu) { return RECOMP_RETURN_NORMAL; }
void I_RESET(CpuState *cpu) { (void)cpu; }
static RecompReturn helper_not_top_level(CpuState *cpu) { return 0; }
"""


def test_collect_bank_symbols_reduces_variants_to_bases():
    bases = collect_bank_symbols(_SAMPLE_BANK)
    assert bases == ["GameMode08_FileSelect", "I_RESET", "bank_00_9CB0"], bases
    # A callee that is only CALLED here (defined in another bank) is not a
    # definition of this bank; the namespace still covers it because the
    # defining bank contributes the base name.
    assert "GameMode08_FileSelect_Entry3" not in bases


def test_namespace_renames_bases_variants_and_module_globals():
    header = emit_module_namespace("smas", ["I_RESET", "Foo"])
    defines = dict(re.findall(r"^#define (\w+) (\w+)$", header, re.M))
    for name in ("Foo", "Foo_M0X0", "Foo_M0X1", "Foo_M1X0", "Foo_M1X1",
                 "I_RESET", "I_RESET_M1X1", "g_dispatch_table",
                 "g_dispatch_table_count", "g_ram_routine_guards",
                 "g_ram_routine_guard_count", "g_program_module"):
        assert defines.get(name) == f"smas_{name}", (name, defines.get(name))
    assert "#pragma once" in header
    # Deterministic output: same input, byte-identical header.
    assert header == emit_module_namespace("smas", ["Foo", "I_RESET"])


def test_descriptor_registers_tables_and_identity():
    digest = "ab" * 32
    src = emit_module_descriptor("smas", "smas", 0x200000, digest)
    assert '#include "program_module.h"' in src
    assert f'#include "{MODULE_NAMESPACE_HEADER}"' in src
    assert 'g_program_module.id = "smas";' in src
    assert 'g_program_module.symbol_prefix = "smas";' in src
    assert "g_program_module.rom_size = 2097152u;" in src
    assert "snes_program_module_register(&g_program_module);" in src
    assert src.count("0xab") == 32
    # Unprefixed (stock) module: no namespace include, empty prefix.
    stock = emit_module_descriptor("smw", None, 0x80000, digest)
    assert MODULE_NAMESPACE_HEADER not in stock
    assert 'g_program_module.symbol_prefix = "";' in stock


def test_identity_rules():
    validate_module_identity("smw", None)
    validate_module_identity("smas.smb1", "smas")
    for bad_id in ("", ".hidden", "with space", "x" * 64):
        try:
            validate_module_identity(bad_id, None)
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted bad id {bad_id!r}")
    for bad_prefix in ("", "1abc", "a-b", "x" * 33):
        try:
            validate_module_identity("ok", bad_prefix)
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted bad prefix {bad_prefix!r}")


def test_descriptor_rejects_short_digest():
    try:
        emit_module_descriptor("x", None, 1, "abcd")
    except ValueError:
        return
    raise AssertionError("short digest accepted")


def test_dispatch_table_joins_the_module_namespace():
    manifest = ProgramManifest(roots=(), nodes={})
    plain = emit_dispatch_table(manifest, {}, {}, {})
    prefixed = emit_dispatch_table(manifest, {}, {}, {}, module_prefix="smas")
    assert MODULE_NAMESPACE_HEADER not in plain
    assert f'#include "{MODULE_NAMESPACE_HEADER}"' in prefixed
    assert "const DispatchEntry g_dispatch_table[]" in prefixed
