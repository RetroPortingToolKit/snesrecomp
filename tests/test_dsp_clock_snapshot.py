"""Execute current-build per-clock DSP snapshot and continuation."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_dsp_clock_snapshot(tmp_path):
    compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
    if os.name == "nt" and Path("C:/msys64/mingw64/bin/gcc.exe").is_file():
        compiler = "C:/msys64/mingw64/bin/gcc.exe"
    if not compiler:
        pytest.skip("C compiler required")
    exe = tmp_path / ("snapshot.exe" if os.name == "nt" else "snapshot")
    sources = ["tests/audio/dsp_clock_snapshot_test.c", "runner/src/snes/dsp.c",
        "runner/src/snes/dsp_clock.c", "runner/src/snes/dsp_shadow.c",
        "runner/src/snes/audio_shadow.c", "runner/src/audio_trace.c"]
    gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    build = subprocess.run([compiler, "-std=c11", "-O1", "-DSNESRECOMP_TRACE=0",
        "-ffunction-sections", "-fdata-sections", "-Irunner/src", "-Irunner/src/snes",
        *sources, gc, "-lm", "-o", str(exe)], cwd=ROOT, capture_output=True,
        text=True, timeout=60)
    assert build.returncode == 0, build.stdout + build.stderr
    for model in ("1", "0"):
        env = dict(os.environ, SNESRECOMP_DSP_PER_CLOCK=model)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, env=env)
        assert run.returncode == 0, run.stdout + run.stderr
