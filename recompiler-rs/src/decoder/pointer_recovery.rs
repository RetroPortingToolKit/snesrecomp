//! Bounded candidates for a guarded runtime-pointer match. Python peer:
//! recompiler/v2/pointer_recovery.py. Never re-decode at guessed M/X.
use super::*;

pub(super) fn recover(
    rom: &[u8], mapping: RomMapping, graph: &FunctionDecodeGraph,
    site: &DecodeKey, pointer: u32, long_pointer: bool,
    data: Option<&[(u32, u32, u32)]>, reloc: &[RelocRegion],
) -> Option<Vec<u32>> {
    let mut chain: Vec<&Insn> = Vec::new();
    let mut current = site;
    let mut visited = HashSet::new();
    while chain.len() < 256 {
        let preds: Vec<_> = graph.insns().iter()
            .filter(|d| d.successors.contains(current)).collect();
        if preds.is_empty() { break; }
        if preds.len() != 1 { return None; }
        let pred = preds[0];
        if !visited.insert(pred.key.clone()) ||
            (pred.key.pc & 0xffff) + u32::from(pred.insn.length) != (current.pc & 0xffff) {
            return None;
        }
        if pred.successors.len() != 1 || matches!(pred.insn.mnem, "JSR" | "JSL") { break; }
        chain.push(&pred.insn);
        current = &pred.key;
    }
    chain.reverse();
    if chain.is_empty() { return None; }
    let bound = chain.iter()
        .filter(|i| matches!(i.mnem, "LDX" | "LDY") && i.mode == Mode::Imm)
        .map(|i| i.operand).last().or(if site.x != 0 { Some(255) } else { None })?;
    if bound > 255 { return None; }
    let bank = site.pc >> 16;
    let mut targets = Vec::new();
    for seed in 0..=bound {
        let mut regs: [Option<u32>; 3] = [None, Some(seed), Some(seed)];
        let mut slots: HashMap<u32, Option<u32>> = HashMap::new();
        for i in &chain {
            let name = i.mnem;
            let v = i.operand;
            let amask = if i.m_flag != 0 { 255 } else { 65535 };
            let xmask = if i.x_flag != 0 { 255 } else { 65535 };
            match (name, i.mode) {
                ("REP" | "SEP" | "NOP" | "PHP" | "PLP", _) => {},
                ("LDX" | "LDY", Mode::Imm) => {
                    regs[if name == "LDX" { 1 } else { 2 }] = if v <= 255 { Some(seed) } else { None };
                },
                ("LDA", Mode::Imm) => regs[0] = Some(v & amask),
                ("LDA", Mode::AbsX | Mode::AbsY | Mode::LongX) => {
                    let idx = regs[if i.mode == Mode::AbsY { 2 } else { 1 }]?;
                    let address = v + idx;
                    let read_bank = if i.mode == Mode::LongX { address >> 16 } else { bank };
                    let address = address & 0xffff;
                    let off = try_rom_offset(mapping, read_bank, address, reloc)?;
                    let size = if i.m_flag != 0 { 1 } else { 2 };
                    if off + size > rom.len() || address + size as u32 > 0x10000 { return None; }
                    regs[0] = Some(rom[off] as u32 |
                        if size == 2 { (rom[off + 1] as u32) << 8 } else { 0 });
                },
                ("TAX" | "TAY", _) => regs[if name == "TAX" { 1 } else { 2 }] = regs[0].map(|a| a & xmask),
                ("TXA" | "TYA", _) => regs[0] = regs[if name == "TXA" { 1 } else { 2 }].map(|a| a & amask),
                ("INX" | "DEX" | "INY" | "DEY", _) => {
                    let r = if name.ends_with('X') { 1 } else { 2 };
                    regs[r] = regs[r].map(|a| if name.starts_with("IN") {
                        a.wrapping_add(1) & xmask
                    } else { a.wrapping_sub(1) & xmask });
                },
                ("AND" | "ORA" | "EOR", Mode::Imm) => {
                    regs[0] = regs[0].map(|a| (match name {
                        "AND" => a & v, "ORA" => a | v, _ => a ^ v,
                    }) & amask);
                },
                ("ASL" | "LSR", Mode::Acc) => {
                    regs[0] = regs[0].map(|a| (if name == "ASL" { a << 1 } else { a >> 1 }) & amask);
                },
                ("STA" | "STX" | "STY" | "STZ", Mode::Dp) => {
                    let val = match name { "STA" => regs[0], "STX" => regs[1], "STY" => regs[2], _ => Some(0) };
                    let width = if matches!(name, "STX" | "STY") { i.x_flag } else { i.m_flag };
                    for n in 0..if width != 0 { 1 } else { 2 } {
                        slots.insert((v + n) & 0xffff, val.map(|a| (a >> (8*n)) & 255));
                    }
                },
                _ => return None,
            }
        }
        let mut target = 0u32;
        for n in 0..if long_pointer { 3 } else { 2 } {
            target |= slots.get(&(pointer + n)).copied().flatten()? << (8*n);
        }
        if !long_pointer { target |= bank << 16; }
        if target & 0xffff >= 0x8000 &&
            !addr_in_data_regions(data, target >> 16, target & 0xffff) &&
            !dispatch_target_is_padding(rom, mapping, target >> 16, target & 0xffff, reloc) {
            targets.push(target);
        }
    }
    targets.sort_unstable();
    targets.dedup();
    if targets.is_empty() { None } else { Some(targets) }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn packed_pointer_and_widths() {
        let cases: &[(&[u8], u8, u8, &[u32])] = &[
            (&[0xa2,2,0xbd,0,0x90,0xaa,0xbd,0,0x91,0x85,0xfe,0xbd,1,0x91,0x85,0xff,0x6c,0xfe,0],1,1,&[0xa000,0xa010,0xa020]),
            (&[0xa2,0,0,0xbd,0,0x91,0x85,0xfe,0x6c,0xfe,0],0,0,&[0xa000]),
            (&[0xa2,0,0xbd,0,0x91,0x69,1,0x85,0xfe,0xbd,1,0x91,0x85,0xff,0x6c,0xfe,0],1,1,&[]),
        ];
        for &(code,m,x,expected) in cases {
            let mut rom=vec![0xff;0x8000];
            rom[..code.len()].copy_from_slice(code);
            rom[0x1000..0x1003].copy_from_slice(&[0,3,7]);
            rom[0x1100..0x1109].copy_from_slice(&[0,0xa0,0xff,0x10,0xa0,0xff,0xff,0x20,0xa0]);
            for offset in [0x2000,0x2010,0x2020] { rom[offset..offset+3].copy_from_slice(&[0xea,0x60,0xea]); }
            let graph=decode_function(&rom,0,0x8000,m,x,None,&DecodeEnv::default());
            let jump=&graph.insns().iter().find(|i| i.insn.opcode==0x6c).unwrap().insn;
            assert_eq!(jump.dispatch_entries.as_deref().unwrap_or(&[]),expected);
            if !expected.is_empty() { assert!(jump.dispatch_pointer_match); }
        }
    }
    #[test]
    fn authority_rejects_wrong_width_and_interior_entry() {
        let mut rom=vec![0;0x8000];rom[..4].copy_from_slice(&[0xa9,0x60,0,0x60]);
        let authority=HashMap::from([(0x8000,vec![0xa9,0x60,0]),(0x8001,vec![]),(0x8002,vec![]),(0x8003,vec![0x60])]);
        let env=DecodeEnv {authority_bytes:Some(&authority),..Default::default()};
        assert!(!decode_function(&rom,0,0x8000,0,1,None,&env).authority_conflict);
        assert!(decode_function(&rom,0,0x8000,1,1,None,&env).authority_conflict);
        assert!(decode_function(&rom,0,0x8001,1,1,None,&env).authority_conflict);
    }
}
