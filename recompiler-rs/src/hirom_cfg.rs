//! Authored contracts shared by physical HiROM aliases; every execution bank
//! remains a separate analysis context. Mirrors v2/hirom_cfg.py.
use crate::cfg::BankCfg;
use crate::rom::hirom_bank_aliases;
use std::collections::HashSet;

pub fn expand(cfgs: Vec<BankCfg>) -> Vec<BankCfg> {
    let explicit_exits: HashSet<_> = cfgs
        .iter()
        .flat_map(|c| c.exit_mx_at.iter().map(|&(b, pc, _, _)| (b, pc)))
        .collect();
    let explicit_variants: HashSet<_> = cfgs
        .iter()
        .flat_map(|c| {
            c.exit_mx_at_per_variant
                .iter()
                .map(|&(b, pc, m, x, _, _)| (b, pc, m, x))
        })
        .collect();
    let explicit_sets: HashSet<_> = cfgs
        .iter()
        .flat_map(|c| {
            c.exit_mx_set
                .iter()
                .map(|(b, pc, m, x, _)| (*b, *pc, *m, *x))
        })
        .collect();
    let mut result = cfgs.clone();
    for src in &cfgs {
        let bank = src.bank as u32;
        for target in hirom_bank_aliases(bank, 0x8000) {
            if target == bank {
                continue;
            }
            let index = match result.iter().position(|c| c.bank == target as i32) {
                Some(i) => i,
                None => {
                    result.push(BankCfg {
                        bank: target as i32,
                        ..Default::default()
                    });
                    result.len() - 1
                }
            };
            let dst = &mut result[index];
            let aliases = |pc: u32| hirom_bank_aliases(bank, pc).contains(&target);
            let span = |start: u32, end: u32| {
                let floor = if bank & 0x7f < 0x40 || target & 0x7f < 0x40 {
                    0x8000
                } else {
                    0
                };
                let start = start.max(floor);
                (start < end && aliases(start)).then_some((start, end))
            };
            for entry in &src.entries {
                if aliases(entry.start) && !dst.entries.iter().any(|e| e.start == entry.start) {
                    let mut entry = entry.clone();
                    entry.name = None;
                    dst.entries.push(entry);
                }
            }
            for include in &src.includes {
                if !dst.includes.contains(include) {
                    dst.includes.push(include.clone());
                }
            }
            macro_rules! inherit_map {
                ($field:ident) => {
                    for (&pc, value) in &src.$field {
                        if aliases(pc) {
                            dst.$field.entry(pc).or_insert_with(|| value.clone());
                        }
                    }
                };
            }
            inherit_map!(authority_insns);
            inherit_map!(hle_func);
            inherit_map!(hle_dispatch);
            macro_rules! inherit_set {
                ($field:ident) => {
                    dst.$field
                        .extend(src.$field.iter().copied().filter(|pc| aliases(*pc)));
                };
            }
            inherit_set!(terminal_jsr);
            inherit_set!(noreturn_jsr);
            inherit_set!(live_operand);
            inherit_set!(brk_rti);
            inherit_set!(inline_dispatch_loops);
            for &pc in &src.hle_spc_upload {
                if aliases(pc) && !dst.hle_spc_upload.contains(&pc) {
                    dst.hle_spc_upload.push(pc);
                }
            }
            for &(start, end) in &src.exclude_ranges {
                if let Some(range) = span(start, end) {
                    if !dst.exclude_ranges.contains(&range) {
                        dst.exclude_ranges.push(range);
                    }
                }
            }
            for &(start, end) in &src.authority_data {
                if let Some(range) = span(start, end) {
                    if !dst.authority_data.contains(&range) {
                        dst.authority_data.push(range);
                    }
                }
            }
            for site in &src.indirect_dispatch {
                if aliases(site.site_pc16)
                    && !dst
                        .indirect_dispatch
                        .iter()
                        .any(|s| s.site_pc16 == site.site_pc16)
                {
                    dst.indirect_dispatch.push(site.clone());
                }
            }
            for (&pc, &value) in &src.force_variant_at {
                if pc >> 16 == bank && aliases(pc & 0xffff) {
                    dst.force_variant_at
                        .entry(target << 16 | (pc & 0xffff))
                        .or_insert(value);
                }
            }
        }
    }
    for cfg in &mut result {
        cfg.force_lle = cfg
            .force_lle
            .iter()
            .flat_map(|&pc| {
                let mut banks = hirom_bank_aliases(pc >> 16, pc & 0xffff);
                if banks.is_empty() {
                    banks.push(pc >> 16);
                }
                banks.into_iter().map(move |b| b << 16 | (pc & 0xffff))
            })
            .collect();
        macro_rules! expand_exits {
            ($field:ident, ($bank:ident, $pc:ident, $($rest:ident),+), $explicit:ident, $key:expr) => {{
                let mut rows = Vec::new();
                for ($bank, $pc, $($rest),+) in &cfg.$field {
                    let mut banks = hirom_bank_aliases(*$bank as u32, *$pc);
                    if banks.is_empty() { banks.push(*$bank as u32); }
                    for b in banks {
                        let row = (b as u8, *$pc, $($rest.clone()),+);
                        if b != *$bank as u32 && $explicit.contains(&($key)(&row)) { continue; }
                        if !rows.contains(&row) { rows.push(row); }
                    }
                }
                cfg.$field = rows;
            }};
        }
        expand_exits!(exit_mx_at, (bank, pc, m, x), explicit_exits, |r: &(
            u8,
            u32,
            u8,
            u8
        )| (
            r.0, r.1
        ));
        expand_exits!(
            exit_mx_at_per_variant,
            (bank, pc, m, x, em, ex),
            explicit_variants,
            |r: &(u8, u32, u8, u8, u8, u8)| (r.0, r.1, r.2, r.3)
        );
        expand_exits!(
            exit_mx_set,
            (bank, pc, m, x, exits),
            explicit_sets,
            |r: &(u8, u32, u8, u8, Vec<(u8, u8)>)| (r.0, r.1, r.2, r.3)
        );
        let mut regions = Vec::new();
        for &(bank, start, end) in &cfg.data_regions {
            let mut banks = hirom_bank_aliases(bank, start.max(0x8000));
            if banks.is_empty() {
                banks.push(bank);
            }
            for b in banks {
                let lo = start.max(if b & 0x7f < 0x40 { 0x8000 } else { 0 });
                if lo < end && !regions.contains(&(b, lo, end)) {
                    regions.push((b, lo, end));
                }
            }
        }
        cfg.data_regions = regions;
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cfg::parse_bank_cfg;
    use crate::rom::{execution_mirror_bank, RomMapping};

    #[test]
    fn contracts_inherit_only_into_valid_windows_and_preserve_explicit_exits() {
        let low = parse_bank_cfg("bank = 00\nexit_mx_at 009000 1 1\n", "low.cfg").unwrap();
        let full = parse_bank_cfg("bank = C0\nfunc Upper 9000 entry_mx:0,1\nfunc Lower 1000\nforce_lle C09000\nexit_mx_at C09000 0 0\n", "full.cfg").unwrap();
        let expanded = expand(vec![low, full]);
        let low = expanded.iter().find(|c| c.bank == 0).unwrap();
        assert_eq!(low.entries.len(), 1);
        assert_eq!(
            (
                low.entries[0].start,
                low.entries[0].entry_m,
                low.entries[0].entry_x
            ),
            (0x9000, 0, 1)
        );
        let full = expanded.iter().find(|c| c.bank == 0xc0).unwrap();
        assert_eq!(
            full.force_lle,
            [0x009000, 0x409000, 0x809000, 0xc09000].into()
        );
        let exits: Vec<_> = expanded
            .iter()
            .flat_map(|c| c.exit_mx_at.iter().copied())
            .collect();
        assert!(exits.contains(&(0, 0x9000, 1, 1)));
        assert!(!exits.contains(&(0, 0x9000, 0, 0)));
        assert!(exits.contains(&(0xc0, 0x9000, 0, 0)));
        assert!(!exits.contains(&(0xc0, 0x9000, 1, 1)));
    }

    #[test]
    fn physical_aliases_do_not_imply_cross_window_execution_mirrors() {
        for bank in [0, 0x40, 0x80, 0xc0] {
            let mut aliases = hirom_bank_aliases(bank, 0x9000);
            aliases.sort();
            assert_eq!(aliases, [0, 0x40, 0x80, 0xc0]);
        }
        assert_eq!(hirom_bank_aliases(0xc0, 0x1000), [0xc0, 0x40]);
        assert!(hirom_bank_aliases(0, 0x1000).is_empty());
        assert!(hirom_bank_aliases(0x7e, 0x9000).is_empty());
        assert_eq!(execution_mirror_bank(RomMapping::HiRom, 0xc0), Some(0x40));
        assert_eq!(execution_mirror_bank(RomMapping::HiRom, 0), Some(0x80));
        assert_eq!(execution_mirror_bank(RomMapping::HiRom, 0xfe), None);
        assert_eq!(execution_mirror_bank(RomMapping::Sa1, 0), None);
    }
}
