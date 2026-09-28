"""The engine-owned command line (runner/src/host_args.c).

Compiles tests/host/host_args_test.c against the unit and runs it. Missing
toolchain resolves to SKIPPED rather than absent, as for the host clock.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
SRC = REPO / 'runner' / 'src'
TEST = REPO / 'tests' / 'host' / 'host_args_test.c'


def _skip(reason: str) -> None:
    print(f"    SKIPPED: {reason}")


def test_host_args_contract():
    """Compile and run the C test; a non-zero exit is this test failing.

    gnu11, not c11: host_paths.c uses readlink(), which strict ISO C hides."""
    assert TEST.is_file(), f"missing test: {TEST}"
    cc = shutil.which('gcc') or shutil.which('cc') or shutil.which('clang')
    if cc is None or os.name == 'nt':
        _skip("no C compiler on PATH")
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / 'host_args_test'
        build = subprocess.run(
            [cc, '-std=gnu11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
             '-I', str(SRC), str(TEST),
             str(SRC / 'host_args.c'), str(SRC / 'host_paths.c'), '-o', str(exe)],
            capture_output=True, text=True, timeout=300)
        if build.returncode != 0:
            raise AssertionError(f"host_args_test failed to build:\n{build.stderr[-2000:]}")
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        if run.returncode != 0:
            raise AssertionError(
                f"host_args_test exited {run.returncode}\n{run.stdout[-2000:]}\n{run.stderr[-2000:]}")
