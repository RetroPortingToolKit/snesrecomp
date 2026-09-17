"""Every tool another repository invokes by path must still be at that path.

tools/ looks internal from inside this repository and is not. A game's release
workflow runs `python3 snesrecomp/tools/rom_identity.py` directly, so the path
IS the interface, and unlike runner/src there is no include-path trick that can
make a moved script keep resolving -- see tools/README.md.

That makes tools/ the same hazard as the layer-folder reorganisation (0a947f4):
a rename that nothing here objects to, delivered to the games. The difference
is that this one is cheap to gate, because the set is small and declared.

tools/README.md is the declaration, and this test reads it rather than keeping
a second copy. A path that is listed but absent fails here; a path that is
present but unlisted is fine, because most of tools/ genuinely is internal.

What this CANNOT check is the right-hand column -- whether the repos named
beside each path still call it, or whether some repo has started calling
something unlisted. Nothing inside this repository can see that. Adding a
caller in a game repo means adding the row here by hand.
"""
import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
README = REPO / 'tools' / 'README.md'

# A table row whose first cell is a backticked path: | `tools/x.py` | ... |
_ROW = re.compile(r'^\|\s*`(tools/[^`]+)`\s*\|')


def _published() -> list:
    rows = [m.group(1) for m in
            (_ROW.match(line) for line in README.read_text(
                encoding='utf-8').splitlines()) if m]
    assert rows, (
        f'{README.relative_to(REPO)} lists no published tools. Either the '
        'table was emptied or its format changed and this parser did not -- '
        'an empty result must not read as "nothing to check".')
    return rows


def test_published_tools_exist():
    missing = [p for p in _published() if not (REPO / p).exists()]
    assert not missing, (
        f'{len(missing)} published tool path(s) named in tools/README.md are '
        'not there:\n  ' + '\n  '.join(missing) +
        '\n\nOther repositories invoke these by path -- a release workflow '
        'runs them directly. Restore the path, or move it AND update the '
        'callers named in that table, in the same change.')


def test_published_table_has_no_duplicates():
    rows = _published()
    dupes = sorted({p for p in rows if rows.count(p) > 1})
    assert not dupes, (
        'tools/README.md lists the same path twice:\n  ' + '\n  '.join(dupes) +
        '\n\nTwo rows for one path means two callers lists, and a reader '
        'updating one of them will believe they are done.')
