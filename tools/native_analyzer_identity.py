"""Which recompiler-rs sources a built native analyzer came from.

Cargo's target directory is untracked, so it survives a framework update: a
port that bumps its snesrecomp submodule keeps the previous revision's
`snesrecomp-analyze` binary. Reusing it silently generates different C from a
fresh checkout of the same revision (measured on Yoshi's Island: the
2f9a164 binary over a410b7b sources materialized 626 AOT variants where the
a410b7b binary materializes 641).

`build_native_analyzer.py` writes the digest of the sources it compiled beside
the binary; `v2_analyze.ensure_native_analyzer` rebuilds, or refuses, when that
stamp does not match the sources in front of it. The digest covers the same
inputs `v2_emit` folds into its generator digest.
"""
from __future__ import annotations

import hashlib
import pathlib
from typing import Optional

SOURCE_ENTRIES = ("src", "Cargo.toml", "Cargo.lock")
STAMP_SUFFIX = ".source-sha256"


def source_digest(crate: pathlib.Path) -> Optional[str]:
    """SHA-256 of the analyzer sources under `crate`, or None if absent."""
    crate = pathlib.Path(crate)
    files = []
    for entry in SOURCE_ENTRIES:
        path = crate / entry
        if path.is_dir():
            files.extend(p for p in path.rglob("*") if p.is_file())
        elif path.is_file():
            files.append(path)
    if not files:
        return None
    digest = hashlib.sha256()
    for path in sorted(files, key=lambda p: p.relative_to(crate).as_posix()):
        digest.update(path.relative_to(crate).as_posix().encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def stamp_path(binary: pathlib.Path) -> pathlib.Path:
    binary = pathlib.Path(binary)
    return binary.with_name(binary.name + STAMP_SUFFIX)


def read_stamp(binary: pathlib.Path) -> Optional[str]:
    try:
        return stamp_path(binary).read_text(encoding="ascii").strip() or None
    except OSError:
        return None


def write_stamp(binary: pathlib.Path, digest: str) -> None:
    stamp_path(binary).write_text(digest + "\n", encoding="ascii")
