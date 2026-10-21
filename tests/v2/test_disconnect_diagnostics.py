"""Compile the real admission barrier and deterministic timing/reason regressions."""
import os
import pathlib
import shutil
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]


def test_disconnect_diagnostics():
    compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
    if os.name == "nt" and pathlib.Path("C:/msys64/mingw64/bin/gcc.exe").is_file():
        compiler = "C:/msys64/mingw64/bin/gcc.exe"
    if not compiler:
        print("    SKIPPED: no C compiler")
        return
    with tempfile.TemporaryDirectory() as temporary:
        executable = pathlib.Path(temporary) / ("diagnostics.exe" if os.name == "nt" else "diagnostics")
        command = [compiler, "-std=c11", "-O1", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                   "-I", str(REPO / "runner/src"),
                   "-I", str(REPO / "runner/src/lobby"),
                   "-I", str(REPO / "lib/recomp-net/include"),
                   str(REPO / "tests/netplay/disconnect_diagnostics_test.c"), "-o", str(executable)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        assert built.returncode == 0, built.stderr
        tested = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
        assert tested.returncode == 0, tested.stdout + tested.stderr
        assert "disconnect diagnostics: PASS" in tested.stdout
