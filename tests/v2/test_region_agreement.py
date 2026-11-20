"""Same-ROM PAL/NTSC netplay agreement at the first-admit barrier.

Both peers are driven through the real admission barrier in one process: an
explicit PAL override against an NTSC header must refuse with region_mismatch
and zero admitted frames, while a matching pair still admits.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]


def _compiler():
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
    if os.name == "nt":
        for candidate in (
            "D:/Program Files/LLVM/bin/clang.exe",
            "C:/msys64/mingw64/bin/gcc.exe",
        ):
            if pathlib.Path(candidate).is_file():
                compiler = candidate
                break
    return compiler


def test_region_agreement():
    compiler = _compiler()
    if not compiler:
        print("    SKIPPED: no C compiler")
        return
    with tempfile.TemporaryDirectory() as temporary:
        exe = pathlib.Path(temporary) / (
            "region_agreement.exe" if os.name == "nt" else "region_agreement"
        )
        command = [compiler]
        if pathlib.Path(compiler).name.lower().startswith("clang") and os.name == "nt":
            command += ["--target=x86_64-pc-windows-msvc", "-fuse-ld=lld"]
        command += [
            "-std=c11", "-O1", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
            "-I", str(REPO / "runner/src"),
            "-I", str(REPO / "runner/src/lobby"),
            "-I", str(REPO / "lib/recomp-net/include"),
            str(REPO / "tests/netplay/region_agreement_test.c"), "-o", str(exe),
        ]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        assert built.returncode == 0, built.stderr
        tested = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        assert tested.returncode == 0, tested.stdout + tested.stderr
        assert "region agreement: PASS" in tested.stdout
        assert "PAL (from override)" in tested.stdout
        assert "NTSC (from header)" in tested.stdout
