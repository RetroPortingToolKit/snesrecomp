"""tools/wram_xref.py: WRAM references classified, and deduplicated across variants.

ROM-free: one routine assembled into a LoROM bank-0 image, listed in a
manifest at two entry widths. Its first instruction is `REP #$30`, so both
variants decode the rest identically and every site must come out once,
naming both variants.
"""
import json
import pathlib
import subprocess
import sys

from _helpers import make_lorom_bank0  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
if str(REPO / "tools") not in sys.path:
    sys.path.insert(0, str(REPO / "tools"))

import wram_xref  # noqa: E402
from v2.program_analysis import ProgramManifest  # noqa: E402

ROUTINE = bytes([
    0xC2, 0x30,                # REP #$30
    0xAF, 0x34, 0x12, 0x7E,    # LDA $7E1234      exact
    0x8F, 0x34, 0x12, 0x00,    # STA $001234      exact (low-WRAM mirror bank)
    0xAF, 0x10, 0x00, 0x7F,    # LDA $7F0010      exact, bank $7F
    0xAF, 0x00, 0x30, 0x00,    # LDA $003000      not WRAM: $00:3000 is not mirrored
    0x8D, 0x00, 0x02,          # STA $0200        db
    0xEE, 0x00, 0x03,          # INC $0300        db, read-modify-write
    0xBD, 0x00, 0x10,          # LDA $1000,X      base, DB-relative
    0xBF, 0x00, 0x20, 0x7E,    # LDA $7E2000,X    base, exact
    0xAD, 0x00, 0x21,          # LDA $2100        I/O: not reported
    0xAD, 0x00, 0x90,          # LDA $9000        ROM under DB: not reported
    0xA5, 0x10,                # LDA $10          dp, only with --dp
    0xB1, 0x20,                # LDA ($20),Y      dp_pointer, only with --dp
    0xD4, 0x30,                # PEI ($30)        dp read, only with --dp
    0x20, 0x00, 0x90,          # JSR $9000        control transfer: not reported
    0x60,                      # RTS
])

CALLEE = bytes([0x60])         # RTS at $9000


def _manifest(variants):
    nodes = {}
    for m, x in variants:
        key = f"008000:M{m}X{x}"
        nodes[key] = {
            "key": {"pc24": 0x8000, "m": m, "x": x},
            "disposition": "aot_eligible",
            "instruction_count": 1,
            "min_pc24": 0x8000,
            "max_pc24": 0x8000 + len(ROUTINE) - 1,
            "demands": [],
            "reasons": [],
        }
    return {
        "format_version": 3,
        "roots": [{"pc24": 0x8000, "m": m, "x": x} for m, x in variants],
        "nodes": nodes,
        "exit_modes": {"009000:M0X0": {"m": 0, "x": 0}},
        "exit_mode_sets": {},
    }


def _rom():
    return make_lorom_bank0({0x8000: ROUTINE, 0x9000: CALLEE})


def _refs(**kwargs):
    manifest = ProgramManifest.from_dict(_manifest([(0, 0), (1, 1)]))
    return wram_xref.collect(_rom(), manifest, **kwargs)


def _by_site(refs):
    return {r.site_pc24 & 0xFFFF: r for r in refs}


def test_classification_without_direct_page():
    refs = _by_site(_refs())
    assert {pc: (r.address24, r.kind, r.access) for pc, r in refs.items()} == {
        0x8002: (0x7E1234, "exact", "R"),
        0x8006: (0x7E1234, "exact", "W"),
        0x800A: (0x7F0010, "exact", "R"),
        0x8012: (0x7E0200, "db", "W"),
        0x8015: (0x7E0300, "db", "RMW"),
        0x8018: (0x7E1000, "base", "R"),
        0x801B: (0x7E2000, "base", "R"),
    }
    assert refs[0x801B].exact_base and not refs[0x8018].exact_base
    assert "DB" in refs[0x8012].qualification()
    assert refs[0x8002].assumed_d is None


def test_direct_page_carries_its_assumption():
    refs = _by_site(_refs(want_dp=True, assume_d=0x1C00))
    assert (refs[0x8025].address24, refs[0x8025].kind) == (0x7E1C10, "dp")
    assert (refs[0x8027].address24, refs[0x8027].kind) == (0x7E1C20, "dp_pointer")
    assert (refs[0x8029].address24, refs[0x8029].kind) == (0x7E1C30, "dp")
    for pc in (0x8025, 0x8027, 0x8029):
        assert refs[pc].assumed_d == 0x1C00
        assert "D=$1C00" in refs[pc].qualification()


def test_one_reference_per_site_across_variants():
    refs = _refs()
    sites = [r.site_pc24 for r in refs]
    assert len(sites) == len(set(sites))
    for r in refs:
        assert sorted(r.variants) == ["008000:M0X0", "008000:M1X1"]


def test_cli_json_and_addr_filter(tmp_path):
    rom = tmp_path / "fixture.sfc"
    rom.write_bytes(_rom())
    manifest = tmp_path / "program_manifest.json"
    manifest.write_text(json.dumps(_manifest([(0, 0), (1, 1)])))
    out = subprocess.run(
        [sys.executable, str(REPO / "tools" / "wram_xref.py"), "--rom", str(rom),
         "--manifest", str(manifest), "--addr", "1234", "--json", "-"],
        check=True, capture_output=True, text=True).stdout
    payload = json.loads(out)
    assert payload["format_version"] == 1
    assert payload["assumptions"]["d"] is None
    got = [(r["address24"], r["kind"], r["access"], r["site_pc24"])
           for r in payload["references"]]
    assert got == [("7E1234", "exact", "R", "008002"),
                   ("7E1234", "exact", "W", "008006")]
    assert payload["references"][0]["variants"] == ["008000:M0X0", "008000:M1X1"]


def test_cli_human_output_lists_sites_with_qualification(tmp_path):
    rom = tmp_path / "fixture.sfc"
    rom.write_bytes(_rom())
    manifest = tmp_path / "program_manifest.json"
    manifest.write_text(json.dumps(_manifest([(0, 0)])))
    out = subprocess.run(
        [sys.executable, str(REPO / "tools" / "wram_xref.py"), "--rom", str(rom),
         "--manifest", str(manifest), "--addr", "0200", "1000"],
        check=True, capture_output=True, text=True).stdout
    assert "$7E:0200  db" in out and "DB-relative" in out
    assert "$7E:1000  base" in out and "indexed base, DB-relative" in out
    assert "00:8012  STA abs" in out
