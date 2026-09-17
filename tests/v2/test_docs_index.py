"""Every document under docs/ must be reachable from docs/README.md.

Before that index existed, ten documents were linked from nothing at all --
including docs/BENCHMARKING.md and docs/ANALYZER_GAPS_INVENTORY.md, the two
longest in the repository. Nothing was broken; they were simply findable only
by already knowing they were there, which is the failure mode a documentation
folder has instead of a build error.

So the index is checked rather than maintained by good intentions: add a doc
and forget to list it and this fails, naming the file. It also fails on a link
that does not resolve, which is how a rename inside docs/ announces itself.

Deliberately NOT checked: whether a one-line hook still describes its document.
Nothing here can see that.
"""
import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
DOCS = REPO / 'docs'
INDEX = DOCS / 'README.md'

_LINK = re.compile(r'\]\(([A-Za-z0-9_./-]+\.md)\)')


def _linked() -> set:
    links = set(_LINK.findall(INDEX.read_text(encoding='utf-8')))
    assert links, (
        'docs/README.md contains no markdown links. Either it was emptied or '
        'its format changed and this parser did not -- an empty result must '
        'not read as "everything is indexed".')
    return links


def test_every_doc_is_indexed():
    present = {p.name for p in DOCS.glob('*.md')} - {'README.md'}
    missing = sorted(present - _linked())
    assert not missing, (
        f'{len(missing)} document(s) under docs/ are not listed in '
        'docs/README.md:\n  ' + '\n  '.join(missing) +
        '\n\nAn unlisted doc is reachable only by knowing it exists. Add a row '
        'with a one-line hook, or delete the file.')


def test_index_links_resolve():
    broken = sorted(l for l in _linked() if not (DOCS / l).exists())
    assert not broken, (
        f'{len(broken)} link(s) in docs/README.md do not resolve:\n  '
        + '\n  '.join(broken) +
        '\n\nRelative to docs/. A doc that moved or was renamed leaves this.')
