"""A staged source SDK must generate with its own analyzer, without overrides."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.build_cli import platform_name


@pytest.mark.skipif(shutil.which("cargo") is None, reason="packager requires Rust")
def test_staged_sdk_uses_its_bundled_analyzer(tmp_path):
    stage = tmp_path / "setup with spaces" / "snesrecomp"
    stage.mkdir(parents=True)
    for name in ("tools", "recompiler"):
        shutil.copytree(ROOT / name, stage / name,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copy2(ROOT / "snesrecomp_cli.py", stage)
    shutil.copytree(ROOT / "recompiler-rs/src", stage / "recompiler-rs/src")
    for name in ("Cargo.toml", "Cargo.lock"):
        shutil.copy2(ROOT / "recompiler-rs" / name, stage / "recompiler-rs" / name)
    env = dict(os.environ, SNESRECOMP_ROOT=str(tmp_path / "wrong framework"),
               SNESRECOMP_NATIVE_ANALYZER=str(tmp_path / "wrong analyzer"))
    command = [sys.executable, str(ROOT / "tools/stage_native_analyzer.py"),
               "--stage-root", str(stage)]
    staged = subprocess.run([*command, "--artifact",
                             platform_name().replace("x86_64", "x64")],
                            env=env, capture_output=True, text=True)
    assert staged.returncode == 0, staged.stdout + staged.stderr
    name = "snesrecomp-analyze.exe" if os.name == "nt" else "snesrecomp-analyze"
    assert (stage / "recompiler-rs/target/release" / name).is_file()
    # Exercise the same dependency walk as the real setup pack, including
    # Windows OS imports used by Rust's random-number support.
    git_bash = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Git/bin/bash.exe"
    bash = str(git_bash) if os.name == "nt" and git_bash.is_file() else shutil.which("bash")
    assert bash, "setup packaging requires bash"
    bundled = subprocess.run([bash, (ROOT / "tools/ci/bundle_runtime_libs.sh").as_posix(),
                              "--exe", (stage / "recompiler-rs/target/release" / name).as_posix(),
                              "--build-dir", (ROOT / "recompiler-rs/target/release").as_posix(),
                              "--strict"], env=env, capture_output=True, text=True)
    assert bundled.returncode == 0, bundled.stdout + bundled.stderr
    assert not list((stage / "recompiler-rs/target/release").glob("[Bb][Cc]rypt[Pp]rimitives.dll"))
    smoke = subprocess.run([*command, "--smoke-only"], env=env,
                           capture_output=True, text=True)
    assert smoke.returncode == 0, smoke.stdout + smoke.stderr
    assert "staged setup SDK smoke passed" in smoke.stdout


def test_wrong_platform_rejected_before_staging(tmp_path):
    (tmp_path / "snesrecomp_cli.py").write_text("")
    result = subprocess.run([sys.executable, str(ROOT / "tools/stage_native_analyzer.py"),
                             "--stage-root", str(tmp_path), "--artifact", "wrong-platform"],
                            capture_output=True, text=True)
    assert result.returncode == 2
    assert "cannot package" in result.stderr
    assert not (tmp_path / "recompiler-rs/target").exists()
