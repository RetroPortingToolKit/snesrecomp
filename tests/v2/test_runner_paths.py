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

The second gate here is the other half of the same bug. Three shell harnesses
cannot use ${SNESRECOMP_RUNNER_INCLUDE_DIRS} -- they invoke the compiler
directly, with no CMake to expand it -- so each keeps a hand-written RUNNER_INC
copy of that list. check_runner_paths.py deliberately exempts them from its
"names runner/src as a search path" advice, because a file that builds the full
layer list has already made the choice that advice asks for. Correct, and it
leaves nobody checking that the copy still says what it copied: add a layer
folder to runner.cmake and the C harnesses go on compiling against the old
header set until something fails to find a header it never knew it needed.
test_runner_include_dirs_agree() is what notices.
"""
import pathlib
import re
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


# The shell harnesses that cannot expand the CMake variable and so copy it.
_RUNNER_INC_COPIES = (
    'tests/run_c_tests.sh',
    'tests/interp816/run.sh',
    'tools/rb_sweep.sh',
)

# The two sides do not spell the runner root the same way and cannot: CMake
# has ${SNESRECOMP_RUNNER_ROOT}, the shells have $ROOT, $SNESRC, or a path
# relative to the repo. Both are normalised to the tail ('src', 'src/cpu'),
# which is the only part that is actually being compared.
_TAIL = r'(src(?:/[A-Za-z0-9_]+)*)'
_CMAKE_PATH = re.compile(r'\$\{SNESRECOMP_RUNNER_ROOT\}/' + _TAIL)
_SHELL_PATH = re.compile(r'runner/' + _TAIL)


def _full(tails) -> list:
    """Tails back to repo-relative paths, for a message someone can act on."""
    return ['runner/' + t for t in tails]


def _bullets(lines) -> str:
    return '\n  '.join(lines)


def _balanced_parens(text: str, open_at: int) -> str:
    """The text between open_at's '(' and its matching ')'."""
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
            if depth == 0:
                return text[open_at + 1:i]
    raise AssertionError(f'unbalanced parenthesis at offset {open_at}')


def _paths_in_list(text: str, opener: str, pattern) -> set:
    """Every runner-src path inside the list `opener` introduces, as tails."""
    assert opener in text, f'no {opener!r} to parse'
    at = text.index(opener) + len(opener) - 1
    found = set(pattern.findall(_balanced_parens(text, at)))
    assert found, f'{opener!r} parsed to no paths -- the parser is stale'
    return found


def test_runner_include_dirs_agree():
    cmake = (REPO / 'runner' / 'runner.cmake').read_text(encoding='utf-8')
    canonical = _paths_in_list(
        cmake, 'set(SNESRECOMP_RUNNER_INCLUDE_DIRS', _CMAKE_PATH)

    # A layer folder that exists but is on nobody's search path is the same
    # bug arriving from the other direction: runner.cmake's configure-time
    # guard checks every listed folder exists, not that every folder is
    # listed. Top level only -- runner/src/lobby/ws is added by the netplay
    # target alone, deliberately.
    on_disk = {f'src/{d.name}'
               for d in (REPO / 'runner' / 'src').iterdir() if d.is_dir()}
    unlisted = sorted(on_disk - canonical)
    assert not unlisted, (
        f'{len(unlisted)} runner/src layer folder(s) exist but are not in '
        'SNESRECOMP_RUNNER_INCLUDE_DIRS:\n  '
        + _bullets(_full(unlisted))
        + '\n\nA flat #include from one of them will not resolve for a '
        'game. Add them to runner/runner.cmake and to every RUNNER_INC '
        'copy:\n  ' + _bullets(_RUNNER_INC_COPIES))

    drifted = []
    for rel in _RUNNER_INC_COPIES:
        copy = _paths_in_list(
            (REPO / rel).read_text(encoding='utf-8'), 'RUNNER_INC=(',
            _SHELL_PATH)
        missing = sorted(canonical - copy)
        extra = sorted(copy - canonical)
        if missing:
            drifted.append(
                f'{rel}: missing ' + ', '.join(_full(missing)))
        if extra:
            drifted.append(
                f'{rel}: names ' + ', '.join(_full(extra))
                + ', which runner.cmake does not')

    assert not drifted, (
        'RUNNER_INC has drifted from SNESRECOMP_RUNNER_INCLUDE_DIRS in '
        f'{len(drifted)} place(s):\n  ' + _bullets(drifted)
        + '\n\nThese harnesses compile runner sources without CMake, so they '
        'must carry the same layer list by hand. A test that builds against a '
        'header set the game build does not have proves nothing about the '
        'game build.')
