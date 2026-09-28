from _helpers import make_lorom_bank0
from v2.decoder import decode_function


def targets(code, m=1, x=1):
    rom = make_lorom_bank0({
        0x8000: bytes.fromhex(code),
        0x9000: bytes([0, 3, 7]),  # irregular packed-record offsets
        0x9100: bytes.fromhex("00 A0 FF 10 A0 FF FF 20 A0"),
        0xA000: bytes.fromhex("EA 60 EA"),
        0xA010: bytes.fromhex("EA 60 EA"),
        0xA020: bytes.fromhex("EA 60 EA"),
    })
    graph = decode_function(rom, 0, 0x8000, m, x)
    jumps = [d.insn for d in graph.insns.values() if d.insn.opcode == 0x6C]
    return jumps[0]


def test_packed_records_through_remap_use_live_pointer_not_dense_index():
    insn = targets("A2 02 BD 00 90 AA BD 00 91 85 FE BD 01 91 85 FF 6C FE 00")
    assert insn.dispatch_entries == [0xA000, 0xA010, 0xA020]
    assert insn.dispatch_pointer_match


def test_word_load_recovery_uses_real_entry_width():
    insn = targets("A2 00 00 BD 00 91 85 FE 6C FE 00", m=0, x=0)
    assert insn.dispatch_entries == [0xA000]
    assert insn.dispatch_pointer_match


def test_unknown_accumulator_clobber_cannot_reuse_prior_table_load():
    insn = targets("A2 00 BD 00 91 69 01 85 FE BD 01 91 85 FF 6C FE 00")
    assert not getattr(insn, "dispatch_entries", None)


def test_unknown_pointer_write_cannot_reuse_old_low_byte():
    insn = targets("A2 00 BD 00 91 85 FE BD 01 91 85 FF 64 FE 6C FE 00")
    # STZ changes the real pointer. This example still lands at A000; the
    # candidate must be obtained from the write, not the stale table fact.
    assert insn.dispatch_entries == [0xA000]
