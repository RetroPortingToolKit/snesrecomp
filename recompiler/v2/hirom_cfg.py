"""Share authored HiROM contracts without collapsing execution-bank analysis."""
from copy import deepcopy

from snes65816 import get_rom_mapping, rom_bank_aliases
from v2.cfg_loader import BankCfg


def expand_hirom_cfgs(parsed):
    if get_rom_mapping() != 'hirom':
        return parsed
    originals = {bank: (path, cfg) for bank, path, cfg in parsed}
    result = {bank: (path, deepcopy(cfg)) for bank, path, cfg in parsed}
    exit_keys = {'exit_mx_at': 2, 'exit_mx_at_per_variant': 4,
                 'declared_exit_mx_at_per_variant': 4, 'exit_mx_set': 4}
    explicit_exits = {name: {tuple(row[:size]) for _, _, cfg in parsed
                             for row in getattr(cfg, name)}
                      for name, size in exit_keys.items()}
    # Local declarations win. Inherited declarations are selected in stable
    # bank order; no inferred result is ever copied by this preprocessing.
    for bank, path, cfg in parsed:
        for target in rom_bank_aliases(bank, 0x8000):
            if target == bank:
                continue
            if target not in result:
                result[target] = (path, BankCfg(bank=target))
            dst = result[target][1]

            def aliases(pc):
                return target in rom_bank_aliases(bank, pc)

            def span(start, end):
                # HiROM maps either the whole bank or its upper half.
                start = max(start, 0x8000 if target & 0x7F < 0x40 else 0)
                start = max(start, 0x8000 if bank & 0x7F < 0x40 else 0)
                return (start, end) if start < end and aliases(start) else None

            present = {e.start for e in dst.entries}
            for entry in cfg.entries:
                if entry.start not in present and aliases(entry.start):
                    copy = deepcopy(entry)
                    copy.name = None  # one authored friendly symbol, exact-bank bodies
                    dst.entries.append(copy)
                    present.add(entry.start)
            for name in ('includes',):
                for value in getattr(cfg, name):
                    if value not in getattr(dst, name):
                        getattr(dst, name).append(value)
            for name in ('authority_insns', 'hle_spc_upload', 'hle_func',
                         'hle_dispatch', 'entry_hook', 'mod_hook',
                         'mod_hook_target'):
                for pc, value in getattr(cfg, name).items():
                    if aliases(pc):
                        getattr(dst, name).setdefault(pc, deepcopy(value))
            for name in ('terminal_jsr', 'noreturn_jsr', 'mod_operand', 'brk_rti'):
                getattr(dst, name).update(pc for pc in getattr(cfg, name) if aliases(pc))
            for name in ('exclude_ranges', 'authority_data'):
                for start, end in getattr(cfg, name):
                    clipped = span(start, end)
                    if clipped and clipped not in getattr(dst, name):
                        getattr(dst, name).append(clipped)
            sites = {d['site_pc16'] for d in dst.indirect_dispatch}
            for site in cfg.indirect_dispatch:
                if aliases(site['site_pc16']) and site['site_pc16'] not in sites:
                    dst.indirect_dispatch.append(deepcopy(site))
                    sites.add(site['site_pc16'])
            for pc, value in cfg.force_variant_at.items():
                if pc >> 16 == bank and aliases(pc & 0xFFFF):
                    dst.force_variant_at.setdefault((target << 16) | (pc & 0xFFFF), value)
    # Absolute declarations can live in any bank file. Expand their physical
    # addresses, while retaining their owning file and explicit execution PCs.
    for _bank, (_path, cfg) in result.items():
        cfg.force_lle = {b << 16 | (pc & 0xFFFF) for pc in cfg.force_lle
                         for b in (rom_bank_aliases(pc >> 16, pc & 0xFFFF)
                                   or (pc >> 16,))}
        for name, key_size in exit_keys.items():
            rows = []
            for bank, pc, *rest in getattr(cfg, name):
                for b in rom_bank_aliases(bank, pc) or (bank,):
                    row = (b, pc, *rest)
                    if b != bank and row[:key_size] in explicit_exits[name]:
                        continue
                    if row not in rows:
                        rows.append(row)
            setattr(cfg, name, rows)
        regions = []
        for bank, start, end in cfg.data_regions:
            for b in rom_bank_aliases(bank, max(start, 0x8000)) or (bank,):
                lo = max(start, 0x8000 if b & 0x7F < 0x40 else 0)
                if lo < end and (b, lo, end) not in regions:
                    regions.append((b, lo, end))
        cfg.data_regions = regions
    # Preserve authored symbol ownership; synthetic banks follow originals.
    order = list(originals) + sorted(set(result) - set(originals))
    return [(bank, *result[bank]) for bank in order]
