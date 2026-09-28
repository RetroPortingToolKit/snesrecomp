#!/usr/bin/env python3
"""Run an isolated, reproducible coverage case and audit its JSON + journal.

The case is local JSON with command (argv array), env, timeout, and optional
copies ({source: relative destination}) and evidence (relative file names).
{run} in arguments/env expands to a NEW output directory. Commands execute
directly without a shell. Evidence must match a reference byte for byte before
an instruction reduction is reported as replay-qualified. This is route-local
evidence, never authorization to promote every observed entry.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "recompiler"))
from v2.coverage_profile import load_profiles
from tier2_ingest import audit


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def local_path(directory, name):
    path = (directory / name).resolve()
    if not path.is_relative_to(directory.resolve()) or path == directory.resolve():
        raise ValueError(f"case output escapes run directory: {name}")
    return path


def compare(reference, candidate):
    """Compare the same route/ROM and report exclusive work per processor."""
    reasons = []
    for key in ("case_digest", "rom_sha256"):
        if reference.get(key) != candidate.get(key):
            reasons.append(f"different {key}")
    if reference.get("returncode") != 0 or candidate.get("returncode") != 0:
        reasons.append("unsuccessful process")
    a, b = reference.get("evidence", {}), candidate.get("evidence", {})
    if not a or a != b or any(value is None for value in b.values()):
        reasons.append("missing or different replay evidence")
    if reference.get("warnings") or candidate.get("warnings"):
        reasons.append("capture warnings require review")
    reductions = {}
    for processor in set(reference.get("instructions", {})) | set(candidate.get("instructions", {})):
        before = reference.get("instructions", {}).get(processor, 0)
        after = candidate.get("instructions", {}).get(processor, 0)
        reductions[processor] = {"before": before, "after": after,
                                 "reduction": before - after,
                                 "percent": (100 * (before - after) / before) if before else None}
    return {"replay_matches": not reasons, "reasons": reasons,
            "interpreted_work": reductions,
            "scope": "Only the recorded route and evidence; manual playtest still required"}


def run_case(case, directory):
    directory = directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    expand = lambda value: str(value).replace("{run}", str(directory))
    for source, destination in case.get("copies", {}).items():
        out = local_path(directory, destination)
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, out)
    for name, content in case.get("files", {}).items():
        out = local_path(directory, name)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(expand(content), encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if not k.startswith("SNESRECOMP_")}
    env.update({k: expand(v) for k, v in case.get("env", {}).items()})
    env.update(SNESRECOMP_TIER2_CAPTURE="1",
               SNESRECOMP_TIER2_MANIFEST=str(directory / "coverage.json"),
               SNESRECOMP_TIER2_JOURNAL=str(directory / "coverage.jsonl"))
    command = [expand(v) for v in case["command"]]
    started = time.monotonic()
    with (directory / "run.log").open("wb") as log:
        try:
            completed = subprocess.run(command, cwd=directory, env=env,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       timeout=case.get("timeout", 180))
            rc = completed.returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
    report = {"command": command, "returncode": rc, "seconds": time.monotonic() - started,
              "evidence": {name: digest(local_path(directory, name))
                           if local_path(directory, name).is_file() else None
                           for name in case.get("evidence", [])}, "warnings": [], "instructions": {}}
    log_text = (directory / "run.log").read_text(encoding="utf-8", errors="replace")
    if "script: unknown" in log_text or "invalid SNESRECOMP_INPUT_SCRIPT" in log_text:
        report["warnings"].append("input script was rejected; this is not a valid fuzz replay")
    # The replay identity excludes build paths; use a caller-supplied stable
    # route identity AND hashes of all scripts/saves so build comparisons
    # cannot accidentally compare different input or SRAM bytes.
    route = {"route": case.get("route"), "files": case.get("files", {}),
             "input_env": {k: v for k, v in case.get("env", {}).items()
                           if "INPUT" in k or k.endswith("FRAMES")},
             "inputs": {name: digest(Path(source)) for source, name in case.get("copies", {}).items()
                        if not name.lower().endswith((".exe", ".dll"))}}
    report["case_digest"] = hashlib.sha256(json.dumps(route, sort_keys=True).encode()).hexdigest()
    paths = [directory / "coverage.json", directory / "coverage.jsonl"]
    if not paths[0].is_file():
        report["warnings"].append("missing checkpoint (no complete cost accounting)")
    try:
        profile = load_profiles([p for p in paths if p.is_file()])
        details = audit(profile)
        (directory / "audit.json").write_text(json.dumps(details, indent=2), encoding="utf-8")
        report["warnings"].extend(profile.warnings)
        report["rom_sha256"] = profile.identity.get("rom_sha256")
        report["identity"] = profile.identity
        report["transfer_tuples"] = len(profile.discoveries)
        report["bail_hits"] = sum(r["bail_hits"] for r in profile.discoveries)
        for row in profile.costs:
            processor = row.get("processor", "snes_cpu")
            report["instructions"][processor] = report["instructions"].get(processor, 0) + row["interpreted_instructions"]
    except (ValueError, OSError, KeyError) as exc:
        report["warnings"].append(f"ingest failed: {exc}")
    (directory / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--reference", type=Path, help="prior result.json for the identical route")
    args = parser.parse_args()
    result = run_case(json.loads(args.case.read_text(encoding="utf-8")), args.output)
    if args.reference:
        result["comparison"] = compare(json.loads(args.reference.read_text(encoding="utf-8")), result)
        (args.output / "comparison.json").write_text(json.dumps(result["comparison"], indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["returncode"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
