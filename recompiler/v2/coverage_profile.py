"""Coverage observations, not execution authority.

v2 checkpoints and journal rows contain cumulative counters per capture. Taking
the newest row before summing captures makes imports idempotent even when both
the journal and its checkpoint are supplied. A torn last journal line is ignored;
malformed complete records and incompatible identities are errors.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

COUNTERS = ("observed_hits", "completed_hits", "yielded_hits", "bail_hits",
            "pending_hits", "interpreted_instructions", "guest_cycles")


def pc(value):
    result = value if isinstance(value, int) else int(str(value), 0)
    if not 0 <= result <= 0xFFFFFF:
        raise ValueError(f"PC outside 24-bit address space: {value!r}")
    return result


def canonical_pc(value, mapper):
    """Only collapse the documented upper-half LoROM execution mirror.

    Preserve raw PCs everywhere else, including mapper-dependent SA-1/SDD1
    windows. Numeric similarity is not evidence of an alias.
    """
    value = pc(value)
    if mapper in ("lorom", "superfx", "cx4", "dsp1"):
        if 0x80 <= value >> 16 <= 0xBF and value & 0xFFFF >= 0x8000:
            return value - 0x800000
    return value


def row_key(row):
    return (row.get("processor", "snes_cpu"), row.get("record_kind", "transfer"), pc(row.get("site_pc24", 0)),
            pc(row["target_pc24"]), row.get("entry_mx", "unknown"),
            row.get("emulation"), row.get("site_kind", "unknown"),
            row.get("fallback_reason", "unknown"))


@dataclass
class CoverageProfile:
    identity: dict = field(default_factory=dict)
    discoveries: list = field(default_factory=list)
    costs: list = field(default_factory=list)
    ram_routines: list = field(default_factory=list)
    warnings: list = field(default_factory=list)
    captures: set = field(default_factory=set)
    unsafe_targets: set = field(default_factory=set)
    qualified_targets: set | None = None


def unpack_records(record, depth=0):
    if depth > 8:
        raise ValueError("coverage bundle nesting exceeds 8 levels")
    if isinstance(record, dict) and record.get("schema") == "snesrecomp tier2 bundle v2":
        records = record.get("records")
        if not isinstance(records, list) or not records:
            raise ValueError("coverage bundle needs a nonempty records array")
        for child in records:
            yield from unpack_records(child, depth + 1)
    else:
        yield record


def _records(path):
    raw = Path(path).read_text(encoding="utf-8")
    if str(path).endswith(".jsonl"):
        lines = raw.splitlines(keepends=True)
        for index, line in enumerate(lines):
            try:
                yield from unpack_records(json.loads(line))
            except json.JSONDecodeError:
                if index == len(lines) - 1 and not line.endswith("\n"):
                    return
                raise ValueError(f"invalid journal record {path}:{index + 1}")
    else:
        yield from unpack_records(json.loads(raw))


def load_profiles(paths, *, expected_rom=None, expected_module=None,
                  legacy_rom=None):
    result = CoverageProfile()
    latest = {}
    ram = {}
    identities = set()
    checkpoints, journals = {}, {}
    for path in paths:
        for record in _records(path):
            if not isinstance(record, dict):
                raise ValueError(f"coverage record must be an object: {path}")
            schema = record.get("schema")
            legacy = schema == "snesrecomp tier2 coverage v1"
            if not legacy and schema not in ("snesrecomp tier2 coverage v2",
                                              "snesrecomp tier2 discovery v2"):
                raise ValueError(f"unsupported coverage schema {schema!r} in {path}")
            identity = record.get("identity", {})
            if not isinstance(identity, dict):
                raise ValueError(f"coverage identity must be an object: {path}")
            if legacy:
                if expected_rom and legacy_rom != expected_rom:
                    raise ValueError("legacy profile has no ROM identity; supply "
                                     "--legacy-profile-rom-sha256 for this ROM")
                result.warnings.append(f"{path}: legacy observations; completion, "
                                       "emulation mode and build identity unverified")
                identity = {"rom_sha256": legacy_rom, "mapper": "lorom"}
            elif not identity.get("rom_sha256") or not record.get("capture_id"):
                raise ValueError(f"capture lacks ROM/session identity: {path}")
            if not legacy:
                for name in ("rom_sha256", "program_digest", "build_digest"):
                    value = identity.get(name)
                    if value and not re.fullmatch("[0-9a-f]{64}", str(value)):
                        raise ValueError(f"invalid {name} in {path}")
                if not identity.get("program_digest") and record.get("capture_scope") != "instruction_costs_only":
                    result.warnings.append(f"{path}: generated program identity missing; regenerate the module descriptor")
                if not identity.get("build_digest"):
                    result.warnings.append(f"{path}: executable build identity unavailable")
            if expected_rom and identity.get("rom_sha256") != expected_rom:
                raise ValueError(f"coverage ROM does not match generation ROM: {path}")
            if expected_module and not legacy and identity.get("module_id") != expected_module:
                raise ValueError(f"coverage module does not match generation module: {path}")
            identities.add((identity.get("rom_sha256"), identity.get("module_id"),
                            identity.get("mapper"), identity.get("program_digest"),
                            identity.get("build_digest")))
            if len(identities) > 1:
                raise ValueError("cannot merge captures from different ROMs/modules/mappers/builds")
            result.identity = identity
            capture = record.get("capture_id", f"legacy:{Path(path).resolve()}")
            result.captures.add(capture)
            sequence = int(record.get("sequence", 0))
            if sequence < 0:
                raise ValueError(f"negative sequence in {path}")
            if schema == "snesrecomp tier2 discovery v2":
                journals[capture] = max(sequence, journals.get(capture, -1))
            elif not legacy and record.get("checkpoint_complete"):
                checkpoints[capture] = max(sequence, checkpoints.get(capture, -1))
            result.unsafe_targets.update(pc(v) for v in record.get("unsafe_aot_targets", []))
            if "qualified_aot_targets" in record:
                if result.qualified_targets is None:
                    result.qualified_targets = set()
                # Qualification belongs to the build that was tested. The
                # generator still analyzes each candidate; this is an allowlist.
                result.qualified_targets.update(pc(v) for v in record["qualified_aot_targets"])
            if record.get("overflowed_tuples", 0) or record.get("dropped_cost_samples", 0):
                result.warnings.append(f"{capture}: bounded capture dropped observations")
            if record.get("journal_write_failures", 0):
                result.warnings.append(f"{capture}: journal writes failed")
            if record.get("ram_routines_overflow", 0):
                result.warnings.append(f"{capture}: bounded RAM capture dropped snapshots")
            for name in ("discoveries", "costs", "ram_routines"):
                if not isinstance(record.get(name, []), list):
                    raise ValueError(f"{name} must be an array: {path}")
            rows = record.get("discoveries", []) + record.get("costs", [])
            if schema == "snesrecomp tier2 discovery v2":
                rows = [record["row"]]
            for original in rows:
                if not isinstance(original, dict):
                    raise ValueError(f"coverage row must be an object: {path}")
                row = dict(original)
                if legacy:
                    clean = int(row.get("clean_hits", 0))
                    sightings = row.get("site_kind") in ("call_gap", "goto_gap")
                    row["observed_hits"] = clean if sightings else 0
                    row["completed_hits"] = 0 if sightings else clean
                    row["legacy"] = True
                for name in COUNTERS:
                    row[name] = int(row.get(name, 0))
                    if row[name] < 0:
                        raise ValueError(f"negative {name} in {path}")
                key = capture, row_key(row)
                seq = int(row.get("sequence", record.get("sequence", 0)))
                if seq < 0:
                    raise ValueError(f"negative sequence in {path}")
                old = latest.get(key)
                if old is None or seq > old[0]:
                    latest[key] = (seq, row)
                elif seq == old[0] and row != old[1]:
                    raise ValueError(f"conflicting coverage row at sequence {seq}: {path}")
            for row in record.get("ram_routines", []):
                if not isinstance(row, dict):
                    raise ValueError(f"RAM snapshot must be an object: {path}")
                key = (capture, pc(row["entry_pc24"]), row.get("hash"),
                       row.get("entry_mx"), row.get("emulation"))
                previous = ram.get(key)
                if previous is None or sequence > previous[0]:
                    ram[key] = (sequence, row)
                elif sequence == previous[0] and row != previous[1]:
                    raise ValueError(f"conflicting RAM snapshot at sequence {sequence}: {path}")
    for capture, sequence in journals.items():
        if sequence > checkpoints.get(capture, -1):
            result.warnings.append(f"{capture}: journal extends beyond the last complete checkpoint; costs may be incomplete")
    merged = {}
    for _seq, row in latest.values():
        key = row_key(row)
        if key not in merged:
            merged[key] = dict(row)
        else:
            out = merged[key]
            for name in COUNTERS:
                out[name] += row[name]
            out["first_frame"] = min(out.get("first_frame", 0), row.get("first_frame", 0))
            out["last_frame"] = max(out.get("last_frame", 0), row.get("last_frame", 0))
    result.discoveries = [r for k, r in sorted(merged.items(), key=lambda kv: str(kv[0]))
                          if r.get("record_kind", "transfer") == "transfer"]
    result.costs = [r for r in merged.values() if r.get("record_kind") == "instruction"]
    result.ram_routines = [row for _seq, row in ram.values()]
    result.warnings = list(dict.fromkeys(result.warnings))
    return result


def promotion_reason(row, declared, identity):
    """Explain whether a row may seed analysis, never whether it is proven AOT."""
    import re
    if row.get("record_kind", "transfer") != "transfer":
        return "instruction_cost_only"
    if row.get("bail_hits", 0):
        return "bailed_requires_investigation"
    if row.get("emulation") is None and not row.get("legacy"):
        return "missing_emulation_mode"
    if row.get("emulation") not in (0, None):
        return "emulation_mode_requires_analysis_support"
    if not re.fullmatch(r"M[01]X[01]", str(row.get("entry_mx", ""))):
        return "missing_entry_width"
    if not (row.get("observed_hits", 0) or row.get("completed_hits", 0)):
        return "no_execution_evidence"
    target = canonical_pc(row["target_pc24"], identity.get("mapper"))
    boundaries = {canonical_pc(v, identity.get("mapper")) for v in declared}
    if row.get("site_kind") != "call_gap" and target not in boundaries:
        return "landing_requires_function_boundary"
    if target >> 16 in (0x7E, 0x7F):
        return "ram_requires_decoded_snapshot_and_guard"
    return "candidate_requires_analysis_and_replay"
