"""Unit coverage for check_runner_paths.py's brace and glob path forms.

tests/v2/test_runner_paths.py runs the tool against the live tree, which is a
good end-to-end check but a bad regression test for a parsing mode the live
tree no longer exercises once it is clean -- exactly the state a passing brace
or glob test here would otherwise never be run in. These build a synthetic
runner/src and exercise scan_file() directly, the way check_runner_paths.py's
own docstring now documents the three spellings it understands.

Background: the layer-folder reorganisation's path sweep (9448e06) rewrote the
literal string "runner/src/foo.c" wherever it appeared, and so could not see
"runner/src/foo.{c,h}" or "runner/src/foo.*" naming the same file -- both read
as a different string entirely. desktop/widescreen.{c,h} and
debug/cosim_state.{c,h} went stale that way, found only by hand; so did the
first-line comment in tests/guarded_patch/guarded_patch_test.c and
tests/host_mesh/host_mesh_test.c, found only once this module's parsing did.
"""
import pathlib
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / 'tools'))

import check_runner_paths as crp  # noqa: E402


def _fixture():
    """A throwaway runner_src with a moved .c/.h pair and a split stem.

    tmp/runner/src/
      desktop/widescreen.c, widescreen.h   -- the "moved" pair
      alpha/split.c                        -- a stem whose extensions now
      beta/split.h                            live in two different folders
    """
    td = tempfile.TemporaryDirectory()
    root = pathlib.Path(td.name)
    rs = root / 'runner' / 'src'
    (rs / 'desktop').mkdir(parents=True)
    (rs / 'desktop' / 'widescreen.c').write_text('')
    (rs / 'desktop' / 'widescreen.h').write_text('')
    (rs / 'alpha').mkdir()
    (rs / 'alpha' / 'split.c').write_text('')
    (rs / 'beta').mkdir()
    (rs / 'beta' / 'split.h').write_text('')
    return td, root, rs


def _scan(root, rs, text, fix=False):
    unique, ambiguous = crp.basename_map(rs)
    doc = root / 'doc.md'
    doc.write_text(text, encoding='utf-8')
    broken, repaired, advice = crp.scan_file(doc, root, rs, unique, ambiguous, fix)
    return broken, repaired, doc.read_text(encoding='utf-8') if fix else text


def test_brace_form_stale_reports_the_fix():
    td, root, rs = _fixture()
    try:
        broken, repaired, _ = _scan(root, rs, "See `runner/src/widescreen.{c,h}`.\n")
        assert repaired == []
        assert len(broken) == 1
        assert 'runner/src/widescreen.{c,h}' in broken[0]
        assert broken[0].endswith('-> desktop/widescreen.{c,h}')
    finally:
        td.cleanup()


def test_brace_form_current_is_clean():
    td, root, rs = _fixture()
    try:
        broken, repaired, _ = _scan(
            root, rs, "See `runner/src/desktop/widescreen.{c,h}`.\n")
        assert broken == [] and repaired == []
    finally:
        td.cleanup()


def test_brace_form_fix_rewrites_in_place():
    td, root, rs = _fixture()
    try:
        broken, repaired, contents = _scan(
            root, rs, "See `runner/src/widescreen.{c,h}`.\n", fix=True)
        assert broken == []
        assert len(repaired) == 1
        assert contents == "See `runner/src/desktop/widescreen.{c,h}`.\n"
    finally:
        td.cleanup()


def test_brace_form_split_across_folders_is_left_for_a_human():
    td, root, rs = _fixture()
    try:
        broken, repaired, _ = _scan(root, rs, "See `runner/src/split.{c,h}`.\n")
        assert repaired == []
        assert len(broken) == 1
        assert 'different folders' in broken[0]
        assert 'alpha' in broken[0] and 'beta' in broken[0]
    finally:
        td.cleanup()


def test_brace_form_missing_extension_is_reported_not_guessed():
    td, root, rs = _fixture()
    try:
        broken, repaired, _ = _scan(
            root, rs, "See `runner/src/nonexistent_stem.{c,h}`.\n")
        assert repaired == []
        assert len(broken) == 1
        assert 'no such file under runner/src' in broken[0]
    finally:
        td.cleanup()


def test_glob_form_stale_reports_the_fix():
    td, root, rs = _fixture()
    try:
        broken, repaired, _ = _scan(root, rs, "See `runner/src/widescreen.*`.\n")
        assert repaired == []
        assert len(broken) == 1
        assert 'runner/src/widescreen.*' in broken[0]
        assert broken[0].endswith('-> desktop/widescreen.*')
    finally:
        td.cleanup()


def test_glob_form_current_is_clean():
    td, root, rs = _fixture()
    try:
        broken, repaired, _ = _scan(
            root, rs, "See `runner/src/desktop/widescreen.*`.\n")
        assert broken == [] and repaired == []
    finally:
        td.cleanup()


def test_glob_form_fix_rewrites_in_place():
    td, root, rs = _fixture()
    try:
        broken, repaired, contents = _scan(
            root, rs, "See `runner/src/widescreen.*`.\n", fix=True)
        assert broken == []
        assert len(repaired) == 1
        assert contents == "See `runner/src/desktop/widescreen.*`.\n"
    finally:
        td.cleanup()


def test_glob_form_removed_file_reports_broken():
    # The shape docs/LAUNCHER_DESIGN.md's "What was removed" note is in: the
    # stem genuinely does not exist under any extension, so this must report
    # broken rather than silently passing through unmatched.
    td, root, rs = _fixture()
    try:
        broken, repaired, _ = _scan(
            root, rs, "In-tree `runner/src/launcher/launcher_gui.*`.\n")
        assert repaired == []
        assert len(broken) == 1
        assert 'no such file under runner/src' in broken[0]
    finally:
        td.cleanup()


def test_glob_form_ambiguous_stem_is_reported():
    td, root, rs = _fixture()
    try:
        broken, repaired, _ = _scan(root, rs, "See `runner/src/split.*`.\n")
        assert repaired == []
        assert len(broken) == 1
        assert 'ambiguous' in broken[0]
    finally:
        td.cleanup()


def test_plain_single_extension_still_works():
    # The pre-existing form, unaffected by adding the other two.
    td, root, rs = _fixture()
    try:
        broken, repaired, _ = _scan(
            root, rs, "See `runner/src/desktop/widescreen.c`.\n")
        assert broken == [] and repaired == []
        broken, repaired, _ = _scan(root, rs, "See `runner/src/widescreen.c`.\n")
        assert repaired == []
        assert len(broken) == 1
        assert broken[0].endswith('-> desktop/widescreen.c')
    finally:
        td.cleanup()


def test_tool_does_not_flag_its_own_docstring_examples():
    # check_runner_paths.py documents its own three spellings using <file> as
    # the stem specifically so a self-scan finds nothing -- the same
    # convention the original anchor-block comment established. Guards
    # against a future doc edit reintroducing a literal "foo.c" example.
    unique, ambiguous = crp.basename_map(REPO / 'runner' / 'src')
    broken, repaired, _ = crp.scan_file(
        REPO / 'tools' / 'check_runner_paths.py', REPO,
        REPO / 'runner' / 'src', unique, ambiguous, fix=False)
    assert broken == [] and repaired == []
