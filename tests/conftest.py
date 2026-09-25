"""Collect every tests/**/test_*.py with pytest, so no module can be orphaned
by a hand-maintained list.

recompiler/ goes first on sys.path: its `v2` package is what tests import as
`v2.*`. tests/v2/ follows for its sibling helpers (`_helpers`), then tests/
for the top-level modules some v2 tests share.
"""
import pathlib
import sys

TESTS = pathlib.Path(__file__).resolve().parent
REPO = TESTS.parent
for path in (TESTS, TESTS / "v2", REPO / "recompiler"):
    if str(path) in sys.path:
        sys.path.remove(str(path))
    sys.path.insert(0, str(path))

collect_ignore_glob = [
    # SMW differential oracle harness: needs a built harness DLL, the oracle
    # core and captured fixtures. Run by hand through tests/l3/run_l3.py.
    "l3/*",
    # Script that takes the built bridge host as argv[1]; lua.yml drives it.
    "lua/*",
]
