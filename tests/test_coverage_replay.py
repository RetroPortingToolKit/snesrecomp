import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from coverage_replay import compare, local_path


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
    assert not compare(before, after)["replay_matches"]


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
