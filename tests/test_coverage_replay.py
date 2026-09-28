import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from coverage_replay import compare, local_path, run_case


def result(**changes):
    return dict(case_digest="route", rom_sha256="a" * 64, returncode=0,
                evidence={"state.bin": "digest"}, warnings=[],
                instructions={"snes_cpu": 100, "sa1": 200}, **changes)


def test_replay_requires_evidence_and_never_combines_coprocessor_savings():
    before = result()
    after = result()
    after["instructions"]["snes_cpu"] = 50
    report = compare(before, after)
    assert report["replay_matches"]
    assert report["interpreted_work"]["snes_cpu"]["percent"] == 50
    assert report["interpreted_work"]["sa1"]["reduction"] == 0
    after["evidence"] = {}
    rejected = compare(before, after)
    assert not rejected["replay_matches"]
    assert rejected["interpreted_work"]["snes_cpu"]["percent"] is None


@pytest.mark.parametrize("key,value", [("case_digest", "another route"),
    ("rom_sha256", "b" * 64), ("returncode", "timeout"),
    ("warnings", ["input script was rejected"])])
def test_incomparable_runs_do_not_qualify(key, value):
    before, after = result(), result()
    after[key] = value
    assert not compare(before, after)["replay_matches"]


def test_run_outputs_stay_in_isolated_directory(tmp_path):
    with pytest.raises(ValueError, match="escapes"):
        local_path(tmp_path, "../save.srm")


def test_matching_black_frame_cannot_hide_lost_game_activity():
    before, after = result(), result()
    before["activity"] = {"video_changes": 161, "logic_changes": 1209}
    after["activity"] = {"video_changes": 0, "logic_changes": 12}
    report = compare(before, after)
    assert not report["replay_matches"]
    assert report["interpreted_work"]["snes_cpu"]["percent"] is None


def test_route_fails_missing_or_insufficient_activity_even_with_success_exit(tmp_path):
    case = {"command": [sys.executable, "-c", "print('video_changes=0')"],
            "capture": False, "activity_minimums": {"video_changes": 1, "logic_changes": 100}}
    report = run_case(case, tmp_path / "run")
    assert report["returncode"] == 0
    assert report["activity"] == {"video_changes": 0, "logic_changes": None}
    assert len(report["warnings"]) == 2
