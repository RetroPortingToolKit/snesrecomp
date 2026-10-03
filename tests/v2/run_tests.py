#!/usr/bin/env python3
"""Run only the tests/v2 recompiler tests. tests/run_tests.py runs everything.

Extra arguments are passed to pytest.
"""
import pathlib
import sys

THIS = pathlib.Path(__file__).resolve().parent
REPO = THIS.parent.parent

if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main(["-c", str(REPO / "pytest.ini"),
                          "--rootdir", str(REPO), str(THIS), *sys.argv[1:]]))
