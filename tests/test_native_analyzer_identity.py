"""A native analyzer binary is only reused for the sources it was built from.

Cargo's target directory survives a framework update, so a port that bumps its
snesrecomp submodule used to keep analyzing with the previous revision's
binary and silently generate different C from a fresh checkout (Yoshi's Island,
2f9a164 binary over a410b7b sources: 626 AOT variants instead of 641).
"""
import os
import pathlib
import subprocess
import sys

import pytest

REPO = pathlib.Path(__file__).resolve().parent.parent
if str(REPO / "tools") not in sys.path:
    sys.path.insert(0, str(REPO / "tools"))

import native_analyzer_identity as identity  # noqa: E402
import v2_analyze  # noqa: E402

BINARY = "snesrecomp-analyze.exe" if os.name == "nt" else "snesrecomp-analyze"


def _crate(root: pathlib.Path) -> pathlib.Path:
    crate = root / "recompiler-rs"
    (crate / "src" / "bin").mkdir(parents=True)
    (crate / "src" / "bin" / "analyze.rs").write_text("fn main() {}\n")
    (crate / "Cargo.toml").write_text("[package]\nname = \"x\"\n")
    (crate / "Cargo.lock").write_text("version = 3\n")
    return crate


def _binary(crate: pathlib.Path, stamp=None) -> pathlib.Path:
    binary = crate / "target" / "release" / BINARY
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.write_bytes(b"old analyzer")
    if stamp is not None:
        identity.write_stamp(binary, stamp)
    return binary


@pytest.fixture
def repo(tmp_path, monkeypatch):
    monkeypatch.setattr(v2_analyze, "REPO", tmp_path)
    monkeypatch.delenv("SNESRECOMP_NATIVE_ANALYZER", raising=False)
    return tmp_path


def _forbid_build(monkeypatch):
    def run(*_args, **_kwargs):
        raise AssertionError("a current analyzer must not be rebuilt")
    monkeypatch.setattr(v2_analyze.subprocess, "run", run)


def _fake_build(monkeypatch, crate, calls):
    def run(command, **_kwargs):
        calls.append(command)
        binary = crate / "target" / "release" / BINARY
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(b"new analyzer")
        identity.write_stamp(binary, identity.source_digest(crate))
        return subprocess.CompletedProcess(command, 0, "", "")
    monkeypatch.setattr(v2_analyze.subprocess, "run", run)


def test_digest_tracks_every_source_input(tmp_path):
    crate = _crate(tmp_path)
    first = identity.source_digest(crate)
    assert first == identity.source_digest(crate)
    for name in ("src/bin/analyze.rs", "Cargo.toml", "Cargo.lock"):
        path = crate / name
        original = path.read_bytes()
        path.write_bytes(original + b"// edit\n")
        assert identity.source_digest(crate) != first, name
        path.write_bytes(original)
    (crate / "src" / "decoder.rs").write_text("// new module\n")
    assert identity.source_digest(crate) != first
    # Build output is not a source.
    _binary(crate)
    (crate / "src" / "decoder.rs").unlink()
    assert identity.source_digest(crate) == first
    assert identity.source_digest(tmp_path / "missing") is None


def test_current_binary_is_reused_without_building(repo, monkeypatch):
    crate = _crate(repo)
    binary = _binary(crate, identity.source_digest(crate))
    _forbid_build(monkeypatch)
    assert v2_analyze.ensure_native_analyzer() == binary


@pytest.mark.parametrize("stamp", [None, "0" * 64], ids=["unstamped", "other-sources"])
def test_stale_binary_is_rebuilt(repo, monkeypatch, capsys, stamp):
    crate = _crate(repo)
    binary = _binary(crate, stamp)
    calls = []
    monkeypatch.setattr(v2_analyze.shutil, "which", lambda _name: "cargo")
    _fake_build(monkeypatch, crate, calls)
    assert v2_analyze.ensure_native_analyzer() == binary
    assert len(calls) == 1 and calls[0][-1].endswith("build_native_analyzer.py")
    assert binary.read_bytes() == b"new analyzer"
    assert identity.read_stamp(binary) == identity.source_digest(crate)
    assert "rebuilding the native analyzer" in capsys.readouterr().out


def test_stale_binary_without_cargo_is_refused(repo, monkeypatch):
    crate = _crate(repo)
    _binary(crate, "0" * 64)
    monkeypatch.setattr(v2_analyze.shutil, "which", lambda _name: None)
    _forbid_build(monkeypatch)
    with pytest.raises(RuntimeError, match="stale"):
        v2_analyze.ensure_native_analyzer()


def test_binary_without_sources_is_trusted(repo, monkeypatch):
    # A layout that ships only the binary has nothing to compare against.
    binary = _binary(repo / "recompiler-rs")
    _forbid_build(monkeypatch)
    assert v2_analyze.ensure_native_analyzer() == binary


def test_explicit_binary_is_used_as_named(repo, monkeypatch):
    crate = _crate(repo)
    binary = _binary(crate, "0" * 64)
    _forbid_build(monkeypatch)
    monkeypatch.setenv("SNESRECOMP_NATIVE_ANALYZER", str(binary))
    assert v2_analyze.ensure_native_analyzer() == binary.resolve()
    monkeypatch.setenv("SNESRECOMP_NATIVE_ANALYZER", str(repo / "absent"))
    with pytest.raises(FileNotFoundError):
        v2_analyze.ensure_native_analyzer()


def test_source_checkout_cli_leaves_the_binary_to_the_freshness_check(monkeypatch):
    # The CLI used to name the in-repo binary explicitly, which made every
    # `snesrecomp_cli.py generate` bypass the stamp check above.
    monkeypatch.delenv("SNESRECOMP_NATIVE_ANALYZER", raising=False)
    monkeypatch.setenv("SNESRECOMP_ROOT", str(REPO))  # the import sets it
    sys.path.insert(0, str(REPO))
    try:
        import snesrecomp_cli
    finally:
        sys.path.remove(str(REPO))
    assert snesrecomp_cli.resolve_analyzer("auto") == "native"
    assert "SNESRECOMP_NATIVE_ANALYZER" not in os.environ
