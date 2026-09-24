#!/usr/bin/env python3
"""Run the snesrecomp Python regression suite.

pytest collects every tests/**/test_*.py (see tests/conftest.py for the path
setup and the few directories that are not unit tests). There is no module
list to keep in sync: a new test file runs as soon as it exists.

The codegen width lint runs first and aborts the run if it fails.
Extra arguments are passed to pytest (e.g. `-k program_emit`, `-x`).

Exit code: 0 all pass, non-zero otherwise.
"""
import pathlib
import subprocess
import sys

TESTS_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent


def main(argv) -> int:
    lint = REPO_ROOT / "tools" / "lint_codegen_widths.py"
    rc = subprocess.call([sys.executable, str(lint)])
    if rc != 0:
        print("lint_codegen_widths failed - aborting test run")
        return rc
    try:
        import pytest
    except ImportError:
        print("pytest is required: python -m pip install pytest")
        return 2
    return pytest.main(["-c", str(REPO_ROOT / "pytest.ini"),
                        "--rootdir", str(REPO_ROOT), str(TESTS_DIR), *argv])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
