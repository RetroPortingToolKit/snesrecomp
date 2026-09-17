"""Every runner/src path this repo names must exist.

The layer-folder reorganisation of runner/src moved 97 files and updated no
build file. Nothing here failed -- the recompiler suite is pure Python and the
C harnesses were not run -- so the breakage was delivered to the games, where
it surfaced as CMake's "Cannot find source file", one file per target, in the
GAME's repo. Three ports hit it independently before anyone looked upstream.

This test is the gate that was missing: it runs tools/check_runner_paths.py
over the framework and fails on any reference to a runner source that is not
where it says it is. runner.cmake carries the same check at configure time for
whoever builds without running the suite.

Prose is deliberately out of scope (see the tool's --include-docs): a doc may
name a deleted file on purpose, and SNES_ACCURACY_BURNDOWN.md does.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / 'tools'))


def test_runner_src_references_resolve():
    import check_runner_paths

    broken = check_runner_paths.check(REPO, fix=False, quiet=True)
    if broken:
        detail = "\n  ".join(broken)
        raise AssertionError(
            f"{len(broken)} runner/src reference(s) do not resolve:\n"
            f"  {detail}\n\n"
            f"Repair with: python3 tools/check_runner_paths.py --fix")
