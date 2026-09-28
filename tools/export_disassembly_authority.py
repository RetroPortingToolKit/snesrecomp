"""Export byte-verified LoROM instruction authority from matching disassembly.

Reassemble first with the pinned assembler and locally owned ROM assets. The
complete assembled image must match --rom. --classifier points to SMW's
reviewed tools/ingest_smwdisx.py (source/macro/WLA mapping parser); no reference
source, ROM bytes, or generated authority belongs in this repository.

asar65816 requires a wholly 65816 source tree (Super Metroid); unknown macro
bodies stay unknown. usdasm uses its explicit US-ROM source address markers
and excludes its SPC/sound sources. Instruction lengths come from source
boundaries, not from the decoder being audited. Unknown spans remain explicit.
"""
from pathlib import Path
import argparse,hashlib,importlib.util,json,re,sys
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT/'recompiler')]
import snes65816


def assemble_rows(args, d):
    ref=args.source; sym=args.symbols
    labels,files,rows=d.parse_symbols(sym);paths,lines=d.resolve_source_files(ref,files)
    if args.format=='smwdisx':
        kind,arch,*_=d.build_maps(ref,files,rows,lines,paths)
    else:
        # This reference is wholly 65816. Macro-owned addresses remain unknown;
        # a multiline data macro must not be mistaken for an instruction.
        macros,owners=d.find_macros(lines)
        kind={};arch={p:'65816' for p in rows}
        for pc,(fid,n) in rows.items():
            if fid not in lines or not 1<=n<=len(lines[fid]):continue
            if (fid,n) in owners:
                macro=macros[owners[(fid,n)]]
                # A macro consisting only of labels/comments and db/dw/dl
                # has unambiguous data semantics, including blank map rows.
                body=lines[macro.file_id][macro.first:macro.last-1]
                heads=[d.head_word(d.strip_label(d.strip_comment(v))) for v in body]
                kind[pc]='data' if all(h in ('','DB','DW','DL','DD') for h in heads) else 'unknown'
                continue
            kind[pc]=d.statement_kind(d.strip_comment(lines[fid][n-1]),'65816',{}, {})
    result={}
    for pc,(fid,n) in rows.items():
        if pc&0xffff<0x8000 or pc==0xffffff:continue
        text=d.strip_label(d.strip_comment(lines[fid][n-1])) if fid in lines and 1<=n<=len(lines[fid]) else ''
        repeat = bool(d.REP_PREFIX_RE.match(text) or re.match(r'^(?:DEX|DEY|INX|INY|NOP|ASL|LSR|ROL|ROR)\s*#',text,re.I))
        result[pc] = dict(kind=kind.get(pc,'unknown') if arch.get(pc)=='65816' else 'data',source=f'{paths[fid].name}:{n}',text=text,repeat=repeat)
    return result

def annotated_rows(args, d):
    rows={}
    for p in args.source.glob('*.asm'):
        for n,line in enumerate(p.read_text(encoding='utf-8-sig').splitlines(),1):
            m=re.search(r'\|([0-9A-Fa-f]{6})(?::[^|]+)?\|\s*(.*)',line)
            if not m or line.lstrip().startswith(';'):continue
            pc=int(m[1],16);text=d.strip_label(d.strip_comment(m[2]))
            rows[pc]=dict(kind='data' if p.name in ('spc.asm','sound.asm') else d.statement_kind(text,'65816',{},{}),source=f'{p.name}:{n}',text=text,repeat=False)
    return rows

def export(args, d):
    rom=args.rom.read_bytes()
    if snes65816.detect_rom_mapping(rom) != 'lorom':
        raise ValueError('these source adapters require a LoROM image')
    snes65816.set_rom_mapping('lorom')
    assembled=args.assembled_rom.read_bytes()
    if len(assembled)==len(rom)+512:assembled=assembled[512:]
    if assembled!=rom:raise ValueError('assembled reference does not reproduce the complete ROM')
    rows=annotated_rows(args,d) if args.format=='usdasm' else assemble_rows(args,d)
    pcs=sorted(rows);instructions=[];data=[];unknown=[]
    for i,pc in enumerate(pcs):
        row=rows[pc];end=pcs[i+1] if i+1<len(pcs) and pcs[i+1]>>16==pc>>16 else ((pc>>16)+1)<<16
        off=snes65816.lorom_offset(pc>>16,pc&0xffff)
        if off>=len(rom):continue
        if row['kind']=='code':
            # Instruction length comes from assembler/source-address boundaries,
            # never from the decoder being judged. Long/unmapped spans are unknown.
            size=end-pc
            if row['repeat']:
                starts=range(pc,end);size=1
            elif 1<=size<=4:starts=[pc]
            elif re.fullmatch(r'(?:RTS|RTL|RTI|NOP|CLC|SEC|PHP|PLP)',row['text'],re.I):
                starts=[pc];size=1
            else:unknown.append((f'{pc:06X}',size,row));continue
            for start in starts:
                o=off+start-pc
                instructions.append(dict(pc24=f'0x{start:06X}',bytes=rom[o:o+size].hex(),source=row['source']))
        elif row['kind']=='data':
            if data and int(data[-1]['end_pc24'],0)==pc and int(data[-1]['start_pc24'],0)>>16==pc>>16:
                data[-1]['end_pc24']=f'0x{end:06X}'
            else:data.append(dict(start_pc24=f'0x{pc:06X}',end_pc24=f'0x{end:06X}'))
        else:unknown.append((f'{pc:06X}',end-pc,row))
    v=dict(schema='snesrecomp disassembly authority v1',rom_sha256=hashlib.sha256(rom).hexdigest(),instructions=instructions,data_regions=data,unknown_source_spans=unknown)
    source_hash=hashlib.sha256()
    for path in sorted(args.source.rglob('*.asm')):
        source_hash.update(path.relative_to(args.source).as_posix().encode()+b'\0')
        source_hash.update(path.read_bytes())
    v['provenance']={'source_commit':args.source_commit,
        'source_asm_sha256':source_hash.hexdigest(),
        'assembled_sha256':hashlib.sha256(assembled).hexdigest(),
        'assembler_sha256':hashlib.sha256(args.assembler.read_bytes()).hexdigest(),
        'classifier_sha256':hashlib.sha256(args.classifier.read_bytes()).hexdigest(),
        'symbols_sha256':hashlib.sha256(args.symbols.read_bytes()).hexdigest() if args.symbols else None,
        'format':args.format}
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(v,indent=2)+'\n',encoding='utf-8')
    print(f'{len(instructions)} instructions, {len(data)} data regions, {len(unknown)} unknown spans')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--format',required=True,choices=('smwdisx','asar65816','usdasm'))
    for name in ('source','rom','assembled-rom','assembler','classifier','out'):
        p.add_argument('--'+name,required=True,type=Path)
    p.add_argument('--symbols',type=Path,help='Asar 1.91 WLA symbols with address-to-line mapping')
    p.add_argument('--source-commit',required=True,help='Pinned reference revision that was assembled')
    args=p.parse_args()
    if args.out.exists():p.error('out already exists; preserve the previous authority evidence')
    if args.format!='usdasm' and not args.symbols:p.error('Asar formats require --symbols')
    spec=importlib.util.spec_from_file_location('authority_source_classifier',args.classifier)
    d=importlib.util.module_from_spec(spec);sys.modules[spec.name]=d;spec.loader.exec_module(d)
    export(args,d)


if __name__=='__main__':
    main()
