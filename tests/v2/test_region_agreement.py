"""Same-ROM PAL/NTSC netplay agreement before the first admit.

The core fixture pins the region fold, the two-peer verdict and the copy that
names each side's region and source. The barrier fixture drives the real host
admission path for both peers and checks zero admits / zero guest frames on a
mismatch, with a matching-region control that still admits. The latter needs
the recomp-net headers from the submodule; it is skipped when they are absent.
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


def _build_and_run(sources, name):
    compiler = _compiler()
    if not compiler:
        print("    SKIPPED: no C compiler")
        return None
    with tempfile.TemporaryDirectory() as temporary:
        exe = pathlib.Path(temporary) / (
            name + (".exe" if os.name == "nt" else "")
        )
        command = [compiler]
        if pathlib.Path(compiler).name.lower().startswith("clang") and os.name == "nt":
            command += ["--target=x86_64-pc-windows-msvc", "-fuse-ld=lld"]
        command += [
            "-std=c11", "-O1", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
            "-Wno-deprecated-declarations",  # MSVC headers mark getenv unsafe
            "-I", str(REPO / "runner/src"),
            "-I", str(REPO / "runner/src/lobby"),
            "-I", str(REPO / "lib/recomp-net/include"),
            # Test-only stand-ins for the uninitialized submodule. The real
            # include path above wins when the submodule is checked out.
            "-I", str(REPO / "tests/netplay/stubs"),
        ]
        command += [str(REPO / source) for source in sources]
        command += ["-o", str(exe)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        assert built.returncode == 0, built.stderr
        tested = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        assert tested.returncode == 0, tested.stdout + tested.stderr
        return tested.stdout


def test_region_agreement_core():
    output = _build_and_run(
        [
            "tests/netplay/region_agreement_core_test.c",
            "runner/src/netplay/snes_netplay_region.c",
        ],
        "region_agreement_core",
    )
    if output is None:
        return
    assert "region agreement core: PASS" in output
    assert "PAL (from override)" in output
    assert "NTSC (from header)" in output


def test_region_agreement_barrier():
    output = _build_and_run(
        [
            "tests/netplay/region_agreement_test.c",
            "runner/src/netplay/snes_netplay_region.c",
        ],
        "region_agreement",
    )
    if output is None:
        return
    assert "region agreement: PASS" in output
    assert "PAL (from override)" in output
    assert "NTSC (from header)" in output
