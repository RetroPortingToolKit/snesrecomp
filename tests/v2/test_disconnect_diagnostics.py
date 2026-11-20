"""Compile the real admission barrier and deterministic timing/reason regressions."""
import os
import pathlib
import shutil
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]


def test_disconnect_diagnostics():
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
    if os.name == "nt":
        for candidate in (
            "D:/Program Files/LLVM/bin/clang.exe",
            "C:/msys64/mingw64/bin/gcc.exe",
        ):
            if pathlib.Path(candidate).is_file():
                compiler = candidate
                break
    if not compiler:
        print("    SKIPPED: no C compiler")
        return
    with tempfile.TemporaryDirectory() as temporary:
        executable = pathlib.Path(temporary) / ("diagnostics.exe" if os.name == "nt" else "diagnostics")
        command = [compiler]
        if pathlib.Path(compiler).name.lower().startswith("clang") and os.name == "nt":
            command += ["--target=x86_64-pc-windows-msvc", "-fuse-ld=lld"]
        command += [
            "-std=c11", "-O1", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
            "-Wno-deprecated-declarations",  # MSVC headers mark getenv unsafe
            "-I", str(REPO / "runner/src"),
            "-I", str(REPO / "runner/src/lobby"),
            "-I", str(REPO / "lib/recomp-net/include"),
            "-I", str(REPO / "tests/netplay/stubs"),
            str(REPO / "tests/netplay/disconnect_diagnostics_test.c"), "-o", str(executable)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        assert built.returncode == 0, built.stderr
        tested = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
        assert tested.returncode == 0, tested.stdout + tested.stderr
        assert "disconnect diagnostics: PASS" in tested.stdout
