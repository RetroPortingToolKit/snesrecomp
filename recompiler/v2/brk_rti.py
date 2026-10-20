"""`brk_rti` validation, shared by decode and emission.

Peer: `check_brk_rti_site` in recompiler-rs/src/bin/analyze.rs. Both apply the
same rule with the same wording, so a site the native analyzer accepts is a
site the emitter compiles and vice versa.

The rule: the byte at the site is $00 (BRK), and the native BRK vector at
$00:FFE6 points at a $40 (RTI) in ROM. Only then is the BRK a round trip that
leaves nothing but four stack bytes and the time.
"""
from snes65816 import is_rom_address, rom_offset, vector_table_offset


def _rom_byte(rom: bytes, bank: int, addr: int):
    if not is_rom_address(bank, addr):
        return None
    offset = rom_offset(bank, addr)
    return rom[offset] if 0 <= offset < len(rom) else None


def check_brk_rti_site(rom: bytes, site_pc24: int) -> None:
    """Raise ValueError unless `site_pc24` is a BRK whose vector is a bare RTI."""
    site = site_pc24 & 0xFFFFFF
    if _rom_byte(rom, site >> 16, site & 0xFFFF) != 0x00:
        raise ValueError(f"brk_rti at ${site:06X}: not a BRK in ROM")
    base = vector_table_offset(rom) + 6
    vector = (rom[base] | rom[base + 1] << 8) if base + 1 < len(rom) else 0
    if _rom_byte(rom, 0x00, vector) != 0x40:
        raise ValueError(
            f"brk_rti at ${site:06X}: the native BRK vector ${vector:04X} "
            f"is not a bare RTI")
