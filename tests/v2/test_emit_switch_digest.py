"""Every environment switch that changes emitted bodies must be part of the
generator digest, or the published-output cache serves code built under the
other setting after a switch flips. The switches are discovered by scanning
the emitter's sources, so a new `SNESRECOMP_EMIT_*` read cannot be added
without joining the key."""
import importlib
import pathlib
import re
import sys

_REPO = pathlib.Path(__file__).resolve().parents[2]
if str(_REPO / "tools") not in sys.path:
    sys.path.insert(0, str(_REPO / "tools"))

_SWITCH = re.compile(r"SNESRECOMP_EMIT_[A-Z0-9_]+")


def _switches_read_by_the_emitter():
    names = set()
    for path in sorted((_REPO / "recompiler" / "v2").glob("*.py")):
        names |= set(_SWITCH.findall(path.read_text(encoding="utf-8")))
    return names


def test_every_emitter_switch_is_in_the_cache_key():
    v2_emit = importlib.import_module("v2_emit")
    found = _switches_read_by_the_emitter()
    assert found, "the scan found no emitter switch; the pattern is stale"
    assert found <= set(v2_emit.EMIT_SWITCHES), found - set(v2_emit.EMIT_SWITCHES)


def test_each_switch_changes_the_digest(monkeypatch):
    v2_emit = importlib.import_module("v2_emit")
    for name in v2_emit.EMIT_SWITCHES:
        monkeypatch.delenv(name, raising=False)
    seen = {v2_emit._emit_switch_digest()}
    for name in v2_emit.EMIT_SWITCHES:
        monkeypatch.setenv(name, "1")
        digest = v2_emit._emit_switch_digest()
        assert digest not in seen, name
        seen.add(digest)
        monkeypatch.delenv(name)


def test_legacy_key_text_is_unchanged(monkeypatch):
    """The existing switch keeps its label, so caches built before the
    switch list existed stay valid."""
    v2_emit = importlib.import_module("v2_emit")
    monkeypatch.delenv("SNESRECOMP_EMIT_AOT_DENY_GATE", raising=False)
    assert v2_emit._emit_switch_digest() == "aot_deny_gate=0"
    monkeypatch.setenv("SNESRECOMP_EMIT_AOT_DENY_GATE", "yes")
    assert v2_emit._emit_switch_digest() == "aot_deny_gate=1"
