#!/usr/bin/env python3
"""Audit -- and optionally repair -- references to runner/src source files.

runner/src is organised into layer folders (cpu/, debug/, desktop/, lobby/,
mods/, netplay/, snes/, state/, util/). Moving a file between them is a rename
that touches no content, so nothing in the compiler or the test suite objects:
the damage lands in runner.cmake, in the shell harnesses, in a game repo's own
CMakeLists, and in prose -- each of which names the OLD path and is discovered
one "Cannot find source file" at a time, by whoever configures next.

That happened once already (the layer-folder reorganisation shipped with zero
build-file updates, breaking every game that pulled it), which is why this tool
exists rather than a paragraph in CONTRIBUTING telling people to remember.

    python3 tools/check_runner_paths.py                  # audit the framework
    python3 tools/check_runner_paths.py --fix            # ...and repair it
    python3 tools/check_runner_paths.py --repo ../MyGame # audit a game repo

The layout on disk is the ground truth: a reference is broken when the file it
names is not there, and it is repairable when exactly one file under runner/src
carries that basename. Anything ambiguous is reported and left alone -- this
tool never guesses which of two same-named files you meant.

Three spellings of a path are understood, and a plain rewrite of the
"runner/src/<file>.c" string alone would silently miss the last two -- as it
did for desktop/widescreen.{c,h} and debug/cosim_state.{c,h} after the
layer-folder move, found only by hand:

    runner/src/<file>.c            one file, one extension
    runner/src/<file>.{c,h}        a .c/.h pair (or more) named in one mention
    runner/src/<file>.*            "this stem, whatever extension it has"

A brace mention repairs only when every extension in it lands in the same
folder; a stem that split across folders is reported, not guessed at.
(<file> is deliberate here too -- see the anchor-pattern comment below.)

Exit status: 0 when nothing is broken (or --fix repaired everything), 1 when
broken references remain.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys

# How a path into runner/src is spelled. Each pattern captures the part AFTER
# the anchor, which is what gets rewritten; the anchor itself is preserved
# verbatim so ${VAR} spellings, quoting and $ROOT-relative forms survive.
#
#   runner/src/<file>.c                     -- prose, shell, a game CMakeLists
#   ${SNESRECOMP_RUNNER_ROOT}/src/<file>.c  -- runner.cmake's own spelling
#   ${RUNSNES}/<file>.c                     -- cosim/CMakeLists.txt's alias
#
# (Spelled with <file> on purpose: a literal example path here is a reference
# like any other, and this tool audits itself.)
_ANCHOR_PATTERNS = [
    r"runner/src/",
    r"\$\{SNESRECOMP_RUNNER_ROOT\}/src/",
    r"\$\{RUNSNES\}/",
]

# Extensions this tool tracks. Shared by all three tail forms below, so
# widening it once widens brace and glob matching along with the plain one.
_EXT_ALT = r"c|cc|cpp|h|hh|hpp"

# runner/src/<file>.c -- one file, one extension. The trailing guard stops `.c`
# from matching the first three characters of `.cpp`: without it
# mod_runtime.cpp silently becomes mod_runtime.c + "pp".
_TAIL = r"(?P<rel>[A-Za-z0-9_./-]+\.(?:" + _EXT_ALT + r"))(?![A-Za-z0-9_])"

# runner/src/<file>.{c,h} -- a .c/.h pair (or more) named in one mention rather
# than repeating the stem. THIRD_PARTY_ATTRIBUTION.md and the accuracy docs
# use this throughout, and 9448e06's path sweep could not see it: matching
# the literal "runner/src/<file>.c" string finds nothing in a line that
# reads "runner/src/<file>.{c,h}". desktop/widescreen.{c,h} and debug/cosim_state.{c,h}
# both went stale here, silently, for exactly that reason.
_BRACE_TAIL = (r"(?P<stem>[A-Za-z0-9_./-]+)\.\{(?P<exts>(?:" + _EXT_ALT +
               r")(?:,(?:" + _EXT_ALT + r"))+)\}")

# runner/src/<file>.* -- "this stem, whatever its extension". Seen in "what was
# removed" notes, where no extension resolves because nothing under that stem
# exists any more; existence is checked against every extension above, and
# it is broken only when none of them do.
_GLOB_TAIL = r"(?P<stem>[A-Za-z0-9_./-]+)\.\*(?![A-Za-z0-9_.*])"

ANCHORS = [re.compile(f"(?P<anchor>{a})" + _TAIL) for a in _ANCHOR_PATTERNS]
BRACE_ANCHORS = [re.compile(f"(?P<anchor>{a})" + _BRACE_TAIL)
                 for a in _ANCHOR_PATTERNS]
GLOB_ANCHORS = [re.compile(f"(?P<anchor>{a})" + _GLOB_TAIL)
                for a in _ANCHOR_PATTERNS]

# Tuple form of _EXT_ALT, for the glob form's "does anything resolve" check.
_KNOWN_EXTS = tuple(_EXT_ALT.split("|"))

# A relative #include from inside runner/src, e.g. #include "../types.h".
# Bare-filename includes are deliberately not checked: they resolve through
# SNESRECOMP_RUNNER_INCLUDE_DIRS, and most of them are system headers.
INCLUDE_RE = re.compile(r'#\s*include\s+"(?P<rel>\.\.?/[^"]+)"')

# The other half of the same bug: a consumer that puts runner/src itself on
# the search path and stops there. That was enough when every runner header
# sat in one directory; now it finds none of them. It is not an error (the
# directory does exist, and some targets legitimately want only src/snes), so
# this is advice rather than a failure -- but it is advice worth printing,
# because the symptom is a "No such file or directory" on a header that is
# plainly present.
INCLUDE_DIR_RE = re.compile(
    r"(?:runner/src|\$\{SNESRECOMP_RUNNER_ROOT\}/src)(?![A-Za-z0-9_/.-])")
INCLUDE_DIR_CONTEXT = re.compile(r"include_directories|(?:^|\s)-I(?:\s|$)|INCLUDE_DIRS")

SKIP_DIRS = {".git", "build", "build-release", "node_modules", "__pycache__",
             "third_party", "refs", "gen"}

# What a broken path here actually breaks a build. This set is the gate.
BUILD_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp", ".cmake", ".txt",
                  ".py", ".sh", ".bash", ".yml", ".yaml", ".json", ".ini",
                  ".cfg", ".bat", ".ps1", ".gradle", ".vcxproj", ".filters"}

# Prose, swept only with --include-docs. Deliberately out of the gate: a doc
# is allowed to name a file that does not exist -- docs/SNES_ACCURACY_BURNDOWN.md
# names launcher_gui.c precisely to record that it exists on no checkout, and
# rewriting that sentence would falsify the record rather than fix anything.
DOC_SUFFIXES = {".md", ".glsl"}
TEXT_SUFFIXES = BUILD_SUFFIXES | DOC_SUFFIXES


def find_runner_src(repo: pathlib.Path, explicit=None):
    """Locate runner/src for `repo` -- its own, or its snesrecomp checkout's.

    `explicit` wins when given: a game may build against a framework that is
    not at <repo>/snesrecomp (SNESRECOMP_ROOT, a worktree, a tool driving this
    from outside), and auditing a different framework than the one the build
    actually uses is worse than not auditing.
    """
    if explicit is not None:
        cand = pathlib.Path(explicit).expanduser().resolve()
        if cand.name != "src":
            cand = cand / "runner" / "src"
        return cand if cand.is_dir() else None
    for cand in (repo / "runner" / "src", repo / "snesrecomp" / "runner" / "src"):
        if cand.is_dir():
            return cand
    return None


def basename_map(runner_src: pathlib.Path):
    """{basename: "<layer>/<basename>"} for every source under runner/src.

    Returns (unique, ambiguous). A basename carried by two layer folders is
    not repairable automatically, so it goes in the second dict and any broken
    reference to it is reported for a human.
    """
    seen = {}
    for dirpath, dirnames, filenames in os.walk(runner_src):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        rel_dir = os.path.relpath(dirpath, runner_src)
        for name in filenames:
            if pathlib.Path(name).suffix not in TEXT_SUFFIXES:
                continue
            rel = name if rel_dir == "." else f"{rel_dir}/{name}"
            seen.setdefault(name, []).append(rel.replace(os.sep, "/"))
    unique = {k: v[0] for k, v in seen.items() if len(v) == 1}
    ambiguous = {k: sorted(v) for k, v in seen.items() if len(v) > 1}
    return unique, ambiguous


def iter_files(repo: pathlib.Path, suffixes):
    """Tracked text files, falling back to a walk outside a git checkout."""
    try:
        out = subprocess.run(["git", "-C", str(repo), "ls-files"],
                             capture_output=True, text=True, check=True).stdout
        names = out.split("\n")
    except (subprocess.CalledProcessError, FileNotFoundError):
        names = []
        for dirpath, dirnames, filenames in os.walk(repo):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for n in filenames:
                names.append(os.path.relpath(os.path.join(dirpath, n), repo))
    for name in names:
        if not name:
            continue
        p = repo / name
        if p.suffix not in suffixes or not p.is_file():
            continue
        if any(part in SKIP_DIRS for part in p.relative_to(repo).parts[:-1]):
            continue
        yield p


def scan_file(path: pathlib.Path, repo: pathlib.Path, runner_src: pathlib.Path,
              unique: dict, ambiguous: dict, fix: bool):
    """Report (and optionally repair) broken runner/src references in `path`.

    Returns (broken, repaired, advice) as lists of human-readable findings.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return [], [], []

    broken = []
    repaired = []
    advice = []
    new_text = text
    inside_runner_src = runner_src in path.parents
    rel_path = path.relative_to(repo)

    def line_of(idx: int) -> int:
        return text.count("\n", 0, idx) + 1

    def repair(rel: str):
        """New path under runner/src for a broken `rel`, or None."""
        return unique.get(os.path.basename(rel))

    def why_unrepairable(rel: str) -> str:
        name = os.path.basename(rel)
        if name in ambiguous:
            return "ambiguous: " + ", ".join(ambiguous[name])
        return "no such file under runner/src"

    for pattern in ANCHORS:
        def sub(mo):
            rel = mo.group("rel")
            anchor = mo.group("anchor")
            if (runner_src / rel).exists():
                return mo.group(0)
            ln = line_of(mo.start())
            fixed = repair(rel)
            if fixed is None:
                broken.append(f"{rel_path}:{ln}: {anchor}{rel} "
                              f"({why_unrepairable(rel)})")
                return mo.group(0)
            if not fix:
                broken.append(f"{rel_path}:{ln}: {anchor}{rel} -> {fixed}")
                return mo.group(0)
            repaired.append(f"{rel_path}:{ln}: {rel} -> {fixed}")
            return anchor + fixed
        new_text = pattern.sub(sub, new_text)

    for pattern in BRACE_ANCHORS:
        def sub_brace(mo):
            stem = mo.group("stem")
            anchor = mo.group("anchor")
            exts = mo.group("exts").split(",")
            rels = [f"{stem}.{e}" for e in exts]
            if all((runner_src / r).exists() for r in rels):
                return mo.group(0)
            ln = line_of(mo.start())
            braced = "{" + mo.group("exts") + "}"
            # Every extension resolves somewhere -- either it is already
            # there, or repair() names where it moved to. The brace form can
            # only be rewritten as one unit when all of them land in the same
            # folder; if they do not (or one is simply gone), that is for a
            # human to split or fix, not this tool to guess at.
            dirs = set()
            reasons = []
            for r in rels:
                if (runner_src / r).exists():
                    dirs.add(os.path.dirname(r))
                    continue
                fixed = repair(r)
                if fixed is None:
                    reasons.append(f"{r} ({why_unrepairable(r)})")
                else:
                    dirs.add(os.path.dirname(fixed))
            if reasons or len(dirs) != 1:
                detail = "; ".join(reasons) if reasons else (
                    "extensions resolve to different folders: "
                    + ", ".join(sorted(dirs)))
                broken.append(f"{rel_path}:{ln}: {anchor}{stem}.{braced} "
                              f"-- {detail}")
                return mo.group(0)
            new_dir = dirs.pop()
            new_stem = (f"{new_dir}/{os.path.basename(stem)}" if new_dir
                       else os.path.basename(stem))
            if not fix:
                broken.append(f"{rel_path}:{ln}: {anchor}{stem}.{braced} -> "
                              f"{new_stem}.{braced}")
                return mo.group(0)
            repaired.append(f"{rel_path}:{ln}: {stem}.{braced} -> "
                            f"{new_stem}.{braced}")
            return f"{anchor}{new_stem}.{braced}"
        new_text = pattern.sub(sub_brace, new_text)

    for pattern in GLOB_ANCHORS:
        def sub_glob(mo):
            stem = mo.group("stem")
            anchor = mo.group("anchor")
            if any((runner_src / f"{stem}.{e}").exists()
                   for e in _KNOWN_EXTS):
                return mo.group(0)
            ln = line_of(mo.start())
            basename_stem = os.path.basename(stem)
            # Any unique basename under runner_src whose stem matches,
            # whatever extension it carries -- that is what ".*" means.
            dirs = {os.path.dirname(v) for k, v in unique.items()
                    if os.path.splitext(k)[0] == basename_stem}
            if not dirs:
                broken.append(f"{rel_path}:{ln}: {anchor}{stem}.* "
                              f"(no such file under runner/src)")
                return mo.group(0)
            if len(dirs) > 1:
                broken.append(f"{rel_path}:{ln}: {anchor}{stem}.* "
                              f"(ambiguous: {', '.join(sorted(dirs))})")
                return mo.group(0)
            new_dir = dirs.pop()
            new_stem = f"{new_dir}/{basename_stem}" if new_dir else basename_stem
            if not fix:
                broken.append(f"{rel_path}:{ln}: {anchor}{stem}.* -> "
                              f"{new_stem}.*")
                return mo.group(0)
            repaired.append(f"{rel_path}:{ln}: {stem}.* -> {new_stem}.*")
            return f"{anchor}{new_stem}.*"
        new_text = pattern.sub(sub_glob, new_text)

    if inside_runner_src:
        here = path.parent

        def sub_inc(mo):
            rel = mo.group("rel")
            if (here / rel).exists():
                return mo.group(0)
            ln = line_of(mo.start())
            fixed = repair(rel)
            if fixed is None:
                broken.append(f'{rel_path}:{ln}: "{rel}" '
                              f'({why_unrepairable(rel)})')
                return mo.group(0)
            # Spell it from this file's own directory, so the include resolves
            # without depending on the consumer's -I list.
            new_rel = os.path.relpath(runner_src / fixed,
                                      here).replace(os.sep, "/")
            if not fix:
                broken.append(f'{rel_path}:{ln}: "{rel}" -> "{new_rel}"')
                return mo.group(0)
            repaired.append(f"{rel_path}:{ln}: {rel} -> {new_rel}")
            return f'#include "{new_rel}"'
        new_text = INCLUDE_RE.sub(sub_inc, new_text)

    # A file that already builds the full layer list -- the framework's own
    # RUNNER_INC arrays, or anything using the cmake variable -- has made the
    # choice this advises; its first list entry is not a finding.
    #
    # The exemption is about the advice only. Whether a RUNNER_INC copy still
    # matches the list it copied is checked by
    # tests/v2/test_runner_paths.py::test_runner_include_dirs_agree, because
    # nothing here can see the other file.
    names_full_list = ("RUNNER_INC" in text
                       or "SNESRECOMP_RUNNER_INCLUDE_DIRS" in text)
    if path.suffix in {".cmake", ".txt", ".sh", ".bash"} and not names_full_list:
        for ln, line in enumerate(text.splitlines(), 1):
            if not INCLUDE_DIR_CONTEXT.search(line):
                continue
            if INCLUDE_DIR_RE.search(line):
                advice.append(
                    f"{rel_path}:{ln}: names runner/src as a search path; "
                    f"use ${{SNESRECOMP_RUNNER_INCLUDE_DIRS}} so every layer "
                    f"folder is on it")

    if fix and new_text != text:
        path.write_text(new_text, encoding="utf-8")
    return broken, repaired, advice


def check(repo: pathlib.Path, fix: bool = False, quiet: bool = False,
          include_docs: bool = False, runner_src=None):
    """Audit `repo`. Returns the findings left unrepaired."""
    runner_src = find_runner_src(repo, runner_src)
    if runner_src is None:
        raise SystemExit(f"{repo}: no runner/src here or in snesrecomp/ -- "
                         f"pass --repo <a snesrecomp checkout or a game repo>, "
                         f"or --runner-src <framework> to name the framework "
                         f"this repo actually builds against")
    unique, ambiguous = basename_map(runner_src)
    suffixes = TEXT_SUFFIXES if include_docs else BUILD_SUFFIXES

    all_broken = []
    all_repaired = []
    all_advice = []
    for path in iter_files(repo, suffixes):
        broken, repaired, advice = scan_file(path, repo, runner_src, unique,
                                             ambiguous, fix)
        all_broken += broken
        all_repaired += repaired
        all_advice += advice

    if not quiet:
        for line in all_repaired:
            print(f"  fixed  {line}")
        for line in all_broken:
            print(f"  BROKEN {line}")
        for line in all_advice:
            print(f"  advice {line}")
        if all_repaired:
            print(f"\nrepaired {len(all_repaired)} reference(s) against "
                  f"{runner_src}")
        if not all_broken and not all_repaired:
            scope = "build files + docs" if include_docs else "build files"
            print(f"runner/src references OK ({repo}, {scope})")
        sys.stdout.flush()
    # Advice is not part of the gate: a target that wants only src/snes on its
    # search path is making a legitimate choice, and this cannot tell.
    return all_broken


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=None,
                    help="repository to audit (default: this snesrecomp checkout)")
    ap.add_argument("--runner-src", default=None,
                    help="the snesrecomp checkout (or its runner/src) to audit "
                         "against, when the repo does not build against its own "
                         "snesrecomp/ -- e.g. under SNESRECOMP_ROOT")
    ap.add_argument("--fix", action="store_true",
                    help="rewrite repairable references in place")
    ap.add_argument("--include-docs", action="store_true",
                    help="also sweep .md prose (not part of the gate: a doc "
                         "may name a deleted file on purpose)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    repo = (pathlib.Path(args.repo).resolve() if args.repo
            else pathlib.Path(__file__).resolve().parent.parent)
    broken = check(repo, fix=args.fix, quiet=args.quiet,
                   include_docs=args.include_docs,
                   runner_src=args.runner_src)
    if broken:
        if not args.quiet:
            print(f"\n{len(broken)} reference(s) still broken.", file=sys.stderr)
            if not args.fix:
                print("Re-run with --fix to rewrite the repairable ones.",
                      file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
