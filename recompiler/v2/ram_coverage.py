"""Decode bounded RAM snapshots without treating operand bytes as returns."""
import hashlib
import re
from snes65816 import decode_insn, REL, REL16, ABS
from .coverage_profile import pc


def describe_snapshot(row):
    report = dict(row)
    def blocked(reason):
        report["candidate_status"] = reason
        return report
    if row.get("nondeterministic"):
        return blocked("snapshot_changed_requires_runtime_guard")
    match = re.fullmatch(r"M([01])X([01])", row.get("entry_mx", ""))
    if not match or row.get("emulation") != 0:
        return blocked("snapshot_requires_exact_native_entry_mode")
    try:
        raw = bytes.fromhex(row["bytes"])
        start = pc(row["entry_pc24"])
    except (ValueError, KeyError):
        return blocked("invalid_snapshot")
    if not raw or len(raw) > 512 or (start & 0xFFFF) + len(raw) > 0x10000:
        return blocked("snapshot_crosses_bank_or_exceeds_bound")
    pending = [(0, int(match[1]), int(match[2]))]
    visited, owned, returns = set(), {}, set()
    extent = 0
    while pending:
        offset, m, x = pending.pop()
        state = offset, m, x
        if state in visited:
            continue
        if not 0 <= offset < len(raw):
            return blocked("snapshot_control_flow_leaves_capture")
        visited.add(state)
        try:
            insn = decode_insn(raw, offset, (start + offset) & 0xFFFF, start >> 16, m, x)
        except IndexError:
            return blocked("snapshot_truncated_instruction")
        if insn is None or offset + insn.length > len(raw):
            return blocked("snapshot_truncated_instruction")
        for n in range(insn.length):
            if offset + n in owned and owned[offset+n] != (offset, insn.length):
                return blocked("snapshot_conflicting_instruction_boundaries")
            owned[offset+n] = offset, insn.length
        extent = max(extent, offset + insn.length)
        if insn.mnem in ("RTS", "RTL", "RTI"):
            returns.add(insn.mnem)
            continue
        if insn.mnem in ("PLP", "XCE", "JSR", "JSL", "BRK", "COP", "STP", "WAI", "JML"):
            return blocked("snapshot_requires_interprocedural_or_status_analysis")
        if insn.mnem in ("REP", "SEP"):
            if insn.operand & 0x20: m = int(insn.mnem == "SEP")
            if insn.operand & 0x10: x = int(insn.mnem == "SEP")
        if insn.mode in (REL, REL16) or insn.mnem == "JMP":
            if insn.mnem == "JMP" and insn.mode != ABS:
                return blocked("snapshot_indirect_control_flow")
            pending.append((insn.operand - (start & 0xFFFF), m, x))
            if insn.mnem in ("JMP", "BRA", "BRL"):
                continue
        pending.append((offset + insn.length, m, x))
    if not returns:
        return blocked("snapshot_has_no_decoded_return")
    code = raw[:extent]
    hash_value = 2166136261
    for byte in code:
        hash_value = ((hash_value ^ byte) * 16777619) & 0xFFFFFFFF
    report.update(decoded_extent=extent, decoded_instructions=len(visited),
                  return_kinds=sorted(returns), guard_fnv1a=f"0x{hash_value:08X}",
                  snapshot_sha256=hashlib.sha256(code).hexdigest())
    return blocked("decoded_snapshot_requires_guarded_analysis_and_replay")
