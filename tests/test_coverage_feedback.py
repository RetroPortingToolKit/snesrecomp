import hashlib
import json
import pathlib
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "recompiler"))
from v2.coverage_profile import load_profiles, canonical_pc, promotion_reason
from v2.program_emit import discover_profile_roots
from v2.program_analysis import VariantKey


def capture(rows, session="one", seq=3, rom="a" * 64):
    return {"schema": "snesrecomp tier2 coverage v2", "capture_id": session,
            "sequence": seq, "identity": {"rom_sha256": rom, "module_id": "main",
                                         "mapper": "lorom", "program_digest": "b" * 64},
            "discoveries": rows}


def row(**kwargs):
    return dict(site_pc24="0x808000", target_pc24="0x808100", entry_mx="M0X1",
                emulation=0, site_kind="call_gap", observed_hits=1, **kwargs)


def write(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def test_journal_checkpoint_merge_is_idempotent_and_torn_tail_recovers(tmp_path):
    cp = write(tmp_path / "capture.json", capture([row(completed_hits=9, sequence=3)]))
    journal = tmp_path / "capture.jsonl"
    rec = capture([], seq=1)
    rec["schema"] = "snesrecomp tier2 discovery v2"
    rec["row"] = row(pending_hits=1, sequence=1)
    journal.write_text(json.dumps(rec) + "\n" + '{"torn":', encoding="utf-8")
    merged = load_profiles([journal, cp, cp])
    assert len(merged.discoveries) == 1
    assert merged.discoveries[0]["completed_hits"] == 9
    assert merged.discoveries[0]["pending_hits"] == 0


def test_sessions_sum_and_roms_never_mix(tmp_path):
    a = write(tmp_path / "a.json", capture([row()], "one"))
    b = write(tmp_path / "b.json", capture([row()], "two"))
    assert load_profiles([a, b]).discoveries[0]["observed_hits"] == 2
    write(b, capture([row()], "two", rom="c" * 64))
    with pytest.raises(ValueError, match="different ROMs"):
        load_profiles([a, b])


def test_mode_variants_remain_distinct_and_observation_is_not_completion(tmp_path):
    a = row()
    b = dict(a, entry_mx="M1X1")
    path = write(tmp_path / "a.json", capture([a, b]))
    profile = load_profiles([path])
    assert all(r["completed_hits"] == 0 for r in profile.discoveries)
    assert discover_profile_roots([path]) == (
        VariantKey(0x808100, 0, 1), VariantKey(0x808100, 1, 1))


@pytest.mark.parametrize("changes,reason", [
    ({"site_kind": "goto_gap"}, "landing_requires_function_boundary"),
    ({"emulation": 1}, "emulation_mode_requires_analysis_support"),
    ({"bail_hits": 1}, "bailed_requires_investigation"),
    ({"observed_hits": 0, "yielded_hits": 3}, "no_execution_evidence"),
])
def test_ineligible_observations_explain_why(changes, reason):
    observation = row()
    observation.update(changes)
    assert promotion_reason(observation, (), {"mapper": "lorom"}) == reason


def test_aliases_require_mapper_and_rom_window():
    assert canonical_pc(0x808100, "lorom") == 0x008100
    assert canonical_pc(0x807100, "lorom") == 0x807100
    assert canonical_pc(0x808100, "sa1") == 0x808100
    assert canonical_pc(0x808100, "hirom") == 0x808100


def test_legacy_identity_is_an_explicit_generation_input(tmp_path):
    path = write(tmp_path / "legacy.json", {
        "schema": "snesrecomp tier2 coverage v1",
        "discoveries": [dict(row(), clean_hits=1)]})
    with pytest.raises(ValueError, match="legacy profile has no ROM"):
        load_profiles([path], expected_rom="a" * 64)
    profile = load_profiles([path], expected_rom="a" * 64, legacy_rom="a" * 64)
    assert profile.warnings and profile.discoveries[0]["completed_hits"] == 0


def test_corrupt_complete_journal_record_is_not_silently_ignored(tmp_path):
    path = tmp_path / "bad.jsonl"
    path.write_text('{"broken"\n', encoding="utf-8")
    with pytest.raises(ValueError, match="invalid journal"):
        load_profiles([path])


def test_ram_extent_decodes_return_shaped_operands():
    from v2.ram_coverage import describe_snapshot
    snapshot = dict(entry_pc24="0x7E8000", entry_mx="M0X1", emulation=0,
                    bytes="A9604060" + "EA" * 508, nondeterministic=False)
    result = describe_snapshot(snapshot)
    assert result["decoded_extent"] == 4
    assert result["decoded_instructions"] == 2
    assert result["return_kinds"] == ["RTS"]
    assert result["candidate_status"] == "decoded_snapshot_requires_guarded_analysis_and_replay"


def test_ram_branch_beyond_snapshot_cannot_be_promoted():
    from v2.ram_coverage import describe_snapshot
    snapshot = dict(entry_pc24="0x7E8000", entry_mx="M1X1", emulation=0,
                    bytes="801060", nondeterministic=False)
    assert describe_snapshot(snapshot)["candidate_status"] == "snapshot_control_flow_leaves_capture"


def test_instruction_costs_never_mix_processors(tmp_path):
    document = capture([])
    cost = dict(row(), record_kind="instruction", interpreted_instructions=7, guest_cycles=21)
    document["costs"] = [cost, dict(cost, processor="sa1")]
    profile = load_profiles([write(tmp_path / "capture.json", document)])
    assert len(profile.costs) == 2


def test_ram_snapshots_preserve_cpu_mode_variants(tmp_path):
    document = capture([])
    snapshot = dict(entry_pc24="0x7E8000", hash="01234567", entry_mx="M0X1", emulation=0)
    document["ram_routines"] = [snapshot, dict(snapshot, entry_mx="M1X1"),
                                dict(snapshot, emulation=1)]
    assert len(load_profiles([write(tmp_path / "capture.json", document)]).ram_routines) == 3


def test_bundle_roundtrip_preserves_session_identity_and_costs(tmp_path):
    checkpoint = capture([row(completed_hits=9)], seq=3)
    checkpoint["costs"] = [dict(row(), record_kind="instruction", interpreted_instructions=17)]
    journal = dict(capture([], seq=1), schema="snesrecomp tier2 discovery v2", row=row(pending_hits=1))
    bundle = write(tmp_path / "bundle.json", {"schema": "snesrecomp tier2 bundle v2",
                                            "records": [journal, checkpoint, checkpoint]})
    original = write(tmp_path / "original.json", checkpoint)
    profile = load_profiles([bundle, original])
    assert len(profile.captures) == 1
    assert profile.discoveries[0]["completed_hits"] == 9
    assert profile.discoveries[0]["pending_hits"] == 0
    assert profile.costs[0]["interpreted_instructions"] == 17


def test_blacklist_applies_even_when_cfg_already_reaches_target(tmp_path):
    document = capture([row()])
    document["unsafe_aot_targets"] = ["0x008100"]
    denied = set()
    assert not discover_profile_roots([write(tmp_path / "profile.json", document)], (), denied)
    assert denied == {0x008100, 0x808100}


def test_recovered_journal_cannot_claim_complete_instruction_costs(tmp_path):
    checkpoint = dict(capture([row()], seq=3), checkpoint_complete=True)
    journal = dict(capture([], seq=4), schema="snesrecomp tier2 discovery v2", row=row())
    cp = write(tmp_path / "checkpoint.json", checkpoint)
    tail = write(tmp_path / "tail.json", journal)
    assert any("costs may be incomplete" in w for w in load_profiles([cp, tail]).warnings)
    checkpoint["sequence"] = 5
    write(cp, checkpoint)
    assert not any("costs may be incomplete" in w for w in load_profiles([tail, cp]).warnings)


def test_ram_snapshot_import_order_does_not_lose_recent_hits(tmp_path):
    old, new = capture([], seq=1), capture([], seq=4)
    snap = dict(entry_pc24="0x7E8000", hash="12345678", entry_mx="M0X1", emulation=0, hits=1)
    old["ram_routines"] = [snap]
    new["ram_routines"] = [dict(snap, hits=9)]
    a = write(tmp_path / "old.json", old)
    b = write(tmp_path / "new.json", new)
    assert load_profiles([b, a, b]).ram_routines[0]["hits"] == 9


def test_standalone_cost_only_capture_does_not_require_generated_code(tmp_path):
    document = dict(capture([]), capture_scope="instruction_costs_only", checkpoint_complete=True)
    document["identity"].update(program_digest="", build_digest="c" * 64)
    document["costs"] = [dict(row(), record_kind="instruction", interpreted_instructions=17)]
    profile = load_profiles([write(tmp_path / "costs.json", document)])
    assert not profile.warnings
    assert not profile.discoveries
    assert profile.costs[0]["interpreted_instructions"] == 17
