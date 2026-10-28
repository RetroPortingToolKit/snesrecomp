"""Declared mod-owned immediates agree across tiers without modifying ROM."""
import os
import tempfile

import pytest

from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402


def _rom(code: bytes) -> bytes:
    return code + bytes(0x8000 - len(code))


def _load(text):
    fd, path = tempfile.mkstemp(suffix='.cfg', text=True)
    with os.fdopen(fd, 'w', encoding='utf-8') as f:
        f.write(text)
    try:
        return load_bank_cfg(path)
    finally:
        os.unlink(path)


def test_mod_operand_parses():
    assert _load("bank = 00\nmod_operand c236\n").mod_operand == {0xC236}


@pytest.mark.parametrize('text, why', [
    ("bank = 00\nmod_operand c236\nmod_operand c236\n", 'duplicate'),
    ("bank = 00\nmod_operand\n", 'exactly one'),
])
def test_mod_operand_parse_errors(text, why):
    with pytest.raises(ValueError, match=why):
        _load(text)


def test_wide_immediate_reads_mod_owned_value():
    rom = _rom(bytes.fromhex('C9 40 00 60'))          # CMP #$0040 / RTS
    folded = emit_function(rom, bank=0, start=0x8000, entry_m=0, entry_x=0,
                           end=0x8004, func_name='Cmp')
    live = emit_function(rom, bank=0, start=0x8000, entry_m=0, entry_x=0,
                         end=0x8004, func_name='Cmp',
                         mod_operand_sites={0x008000})
    assert '= 0x40;' in folded
    assert '= 0x40;' not in live
    assert 'snes_mod_operand(0x008000u, 0x40)' in live
    assert 'cpu_read16' not in live
    # The cycle charge comes from the opcode and does not move.
    charge = [l for l in folded.splitlines() if 'cpu->cycles +=' in l]
    assert charge == [l for l in live.splitlines() if 'cpu->cycles +=' in l]


def test_narrow_immediate_reads_mod_owned_value():
    rom = _rom(bytes.fromhex('C9 40 60'))             # CMP #$40 / RTS at m=1
    live = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                         end=0x8003, func_name='Cmp',
                         mod_operand_sites={0x008000})
    assert '(uint8)snes_mod_operand(0x008000u, 0x40)' in live


def test_non_immediate_site_is_rejected():
    rom = _rom(bytes.fromhex('A5 10 60'))             # LDA $10 / RTS
    with pytest.raises(ValueError, match='not an immediate'):
        emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                      end=0x8003, func_name='Lda',
                      mod_operand_sites={0x008000})


@pytest.mark.parametrize('code, mnem, why', [
    ('C2 30 60', 'REP', 'a status operand'),           # REP #$30 / RTS
    ('E2 20 60', 'SEP', 'a status operand'),           # SEP #$20 / RTS
    ('54 7E 7F 60', 'MVN', 'not a data operand'),      # MVN $7E,$7F / RTS
])
def test_status_and_control_operands_are_refused(code, mnem, why):
    rom = _rom(bytes.fromhex(code))
    with pytest.raises(ValueError, match=f'{mnem} #imm is {why}'):
        emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                      end=0x8000 + len(bytes.fromhex(code)), func_name='Ctl',
                      mod_operand_sites={0x008000})


# LDA #$00 / BEQ +5 / LDA #$11 / STA $40 / RTS / LDA #$22 / STA $40 / RTS:
# with the immediate folded, Z is known and the fall-through is dead code.
_BEQ = bytes.fromhex('A9 00 F0 05 A9 11 85 40 60 A9 22 85 40 60')


def test_decoder_does_not_fold_a_live_immediate():
    from v2.decoder import decode_function, clear_decode_cache
    rom = _rom(_BEQ)
    clear_decode_cache()
    folded = decode_function(rom, 0, 0x8000, 1, 1, end=0x800E)
    live = decode_function(rom, 0, 0x8000, 1, 1, end=0x800E,
                           mod_operand_sites={0x008000})
    assert [f.branch_pc24 for f in folded.const_z_folds] == [0x008002]
    assert not live.const_z_folds
    pcs = {k.pc & 0xFFFF for k in live.insns}
    assert {0x8004, 0x8009} <= pcs              # both arms kept
    assert 0x8004 not in {k.pc & 0xFFFF for k in folded.insns}


def test_native_analyzer_does_not_fold_a_live_immediate(tmp_path):
    """The manifest must not drop the arm the original bytes made dead."""
    import pathlib
    import sys
    from _helpers import make_lorom_bank0
    repo = pathlib.Path(__file__).resolve().parents[2]
    if str(repo / 'tools') not in sys.path:
        sys.path.insert(0, str(repo / 'tools'))
    from v2_analyze import build_manifest_native
    from v2.program_analysis import VariantKey
    # LDA #$00 / BEQ +4 / JSR $9000 / RTS / JSR $9100 / RTS
    rom = make_lorom_bank0({
        0x8000: bytes.fromhex('A9 00 F0 04 20 00 90 60 20 00 91 60'),
        0x9000: bytes([0x60]), 0x9100: bytes([0x60])})
    rom_path = tmp_path / 'fixture.sfc'
    rom_path.write_bytes(rom)
    targets = {}
    for live in (False, True):
        cfg_dir = tmp_path / ('live' if live else 'folded')
        cfg_dir.mkdir()
        (cfg_dir / 'bank00.cfg').write_text(
            'bank = 00\nfunc Root 8000 end:800c entry_mx:1,1\n'
            + ('mod_operand 8000\n' if live else ''), encoding='utf-8')
        manifest, _helpers, _inline, _out = build_manifest_native(
            rom_path=rom_path, cfg_dir=cfg_dir, all_cfg_roots=True)
        root = manifest.nodes[VariantKey(0x008000, 1, 1)]
        targets[live] = {d.target.pc24 for d in root.demands if d.target}
    assert targets[False] == {0x009100}          # Z=1 folds the BEQ taken
    assert targets[True] == {0x009000, 0x009100}  # both arms analysed



def test_removed_live_operand_is_rejected():
    with pytest.raises(ValueError, match='live_operand is removed'):
        _load('bank = 00\nlive_operand 8000\n')


@pytest.mark.parametrize('site', ['-1', '10000', '038B27', '838B27'])
def test_operand_site_must_be_in_the_cfg_bank(site):
    with pytest.raises(ValueError, match='16-bit'):
        _load(f'bank = 03\nmod_operand {site}\n')


def test_compiled_and_interpreted_mod_values_keep_rom_immutable(tmp_path):
    """Real runtime, cart alias resolver and interpreter; mixed M/X widths.

    Includes the interpreter's optional word-bus path, BIT's separate fetch,
    duplicate registration, clear/reset, undeclared sites and >256 overrides.
    """
    import pathlib
    import subprocess
    import test_hle_func_seam as seam
    from snes65816 import set_rom_mapping, get_rom_mapping
    compiler = seam._compiler()
    if not compiler:
        pytest.skip('C compiler required')
    repo = pathlib.Path(__file__).resolve().parents[2]
    cases = []
    generated = ''
    old = get_rom_mapping()
    try:
        set_rom_mapping('lorom')
        rom = bytearray(0x20000)
        for reg, opcode in [('A', 0xa9), ('X', 0xa2), ('Y', 0xa0), ('BIT', 0x89),
                            ('ADC',0x69),('SBC',0xe9),('AND',0x29),('ORA',0x09),
                            ('EOR',0x49),('CMP',0xc9),('CPX',0xe0),('CPY',0xc0)]:
            for narrow in (0, 1):
                for bank in (3, 0x83):
                    m,x = (1-narrow,narrow) if reg in ('X','Y','CPX','CPY') else (narrow,1-narrow)
                    start=0x8100+len(cases)*0x10
                    code=bytes([opcode,0x42]+([] if narrow else [0x12])+[0x60])
                    rom[0x18000+start-0x8000:0x18000+start-0x8000+len(code)] = code
                    name=f'Case{len(cases)}'
                    generated += emit_function(bytes(rom),bank=bank,start=start,
                        entry_m=m,entry_x=x,end=start+len(code),func_name=name,
                        mod_operand_sites={(bank<<16)|start})
                    cases.append((name,bank,start,m,x,reg,narrow))
    finally:
        set_rom_mapping(old)
    harness=seam._HARNESS.split('/* The C helpers.')[0]
    headers=r"""
#include "mod_operand.h"
#include "snes/snes.h"
#include "snes/interp816.h"
#include "snes/sdd1.h"
#include "snes/sa1.h"
#include <assert.h>
Snes *g_snes;
/* This fixture tests actual LoROM/HiROM cart mapping, without chip devices. */
uint32_t sdd1_mmc_offset(Sdd1 *s, uint32_t a) { (void)s;(void)a;return UINT32_MAX; }
uint32_t sdd1_lorom_window_offset(Sdd1 *s, uint8_t b, uint16_t a) {
  (void)s;(void)b;(void)a;return UINT32_MAX;
}
uint8_t *sa1_cpu_memory_ptr(Sa1 *s, uint8_t b, uint16_t a) {
  (void)s;(void)b;(void)a;return NULL;
}
static unsigned word_reads;
static uint8_t bus_read(void *unused, uint32_t pc) {
  (void)unused;uint8 *p=cart_getRomPtr(g_snes->cart,pc>>16,pc);
  return p ? *p : g_ram[pc&0xffff];
}
static void bus_write(void *unused, uint32_t pc, uint8_t v) {
  (void)unused;g_ram[pc&0xffff]=v;
}
static bool bus_word(void *unused,uint32_t lo,uint32_t hi,uint16_t *v) {
  word_reads++;*v=bus_read(unused,lo)|(bus_read(unused,hi)<<8);return true;
}
"""
    main=['int main(void) {',
          '  uint8 image[0x20000] = {'+', '.join(str(b) for b in rom)+'};',
          '  uint8 original[sizeof image]; memcpy(original,image,sizeof image);',
          '  Cart cart={0};cart.type=CART_LOROM;cart.rom=image;cart.romSize=sizeof image;',
          '  Snes machine={0};machine.cart=&cart;g_snes=&machine;',
          '  assert(!snes_mod_operand_register_sites(NULL,1));',
          '  assert(!snes_mod_operand_set(0x038050,7));',
          '  Interp816 *interpreter=interp816_init(NULL,bus_read,bus_write);',
          '  for(unsigned pass=0;pass<4;pass++) {',
          '    snes_mod_operand_clear_all();',
          '    uint16 value=pass==0 ? 0x1242 : pass==1 ? 0x2363 : 0;',
          '    bool use_mod=pass!=3;']
    for name,bank,start,m,x,reg,narrow in cases:
        main += ['    {',f'      uint32 site=0x{bank:02X}{start:04X};',
                 f'      const uint32 offset=0x{0x18000+start-0x8000:06X};',
                 '      assert(snes_mod_operand_register_sites(&offset,1));',
                 '      if(use_mod) assert(snes_mod_operand_set(site^0x800000,value));',
                 '      uint16 want=use_mod ? value : 0x1242;',
                 '      uint16 got=0xffff;',
                 '      assert(snes_mod_operand_get(site,&got)==use_mod);',
                 '      if(use_mod) assert(got==value);',
                 '      CpuState cpu={0};cpu.ram=g_ram;cpu.S=0x1ff;',
                 f'      cpu.m_flag={m};cpu.x_flag={x};cpu.P={(m<<5)|(x<<4)};',
                 '      cpu.DB=0x7e;cpu.A=0xffff;g_tc_valid=0;g_recomp_stack_top=0;',
                 '      cpu_push_jsr_return_frame(&cpu);',
                 f'      {name}_M{m}X{x}(&cpu);',
                 '      assert(cpu.S==0x1ff && !g_other_exits);',
                 '      memset(interpreter,0,sizeof(*interpreter));',
                 '      interpreter->read=bus_read;interpreter->write=bus_write;',
                 '      interpreter->read_word=bus_word;',
                 f'      interpreter->pc=0x{start:04X};interpreter->k=0x{bank:02X};',
                 f'      interpreter->mf={m};interpreter->xf={x};interpreter->a=0xffff;',
                 '      word_reads=0;interp816_runOpcode(interpreter);']
        if reg=='BIT':
            main += [f'      assert(cpu._flag_Z == ((want & {255 if narrow else 65535})==0));',
                     '      assert(cpu._flag_Z==interpreter->z);']
        elif reg in ('A','X','Y'):
            expect='(0xff00 | (want&0xff))' if reg=='A' and narrow else ('(want&0xff)' if narrow else 'want')
            main += [f'      assert(cpu.{reg}=={expect});',
                     f'      assert(cpu.{reg}==interpreter->{reg.lower()});',
                     f'      assert(word_reads=={0 if narrow else "(use_mod ? 0 : 1)"});']
        else:
            main += ['      assert(cpu.A==interpreter->a && cpu.X==interpreter->x && cpu.Y==interpreter->y);',
                     '      assert(cpu._flag_Z==interpreter->z && cpu._flag_N==interpreter->n);']
        main += ['      assert(!memcmp(original,image,sizeof image));',
                 '      snes_mod_operand_clear(site^0x800000);',
                 '      assert(!snes_mod_operand_get(site,&got));','    }']
    main += ['  }',
             '  uint32 many[1100];for(unsigned i=0;i<1100;i++) many[i]=i;',
             '  assert(snes_mod_operand_register_sites(many,1100));',
             '  for(unsigned i=0;i<1100;i++) assert(snes_mod_operand_set(0x008000+i,i));',
             '  assert(snes_mod_operand_active_count()==1100);',
             '  assert(snes_mod_operand_register_sites(many,1100));',
             '  assert(snes_mod_operand(0x8083ff,0)==0x3ff);',
             '  snes_mod_operand_clear_all();assert(!snes_mod_operand_active_count());',
             '  assert(!memcmp(original,image,sizeof image));',
             '  interp816_free(interpreter);puts("ok");return 0;','}']
    source=tmp_path/'execute.c';source.write_text(harness+headers+generated+'\n'.join(main))
    exe=tmp_path/('execute.exe' if os.name=='nt' else 'execute')
    args=[compiler,'-std=c11','-O1','-flto','-fwhole-program','-ffunction-sections','-fdata-sections',
          '-DSNESRECOMP_TRACE=0','-I',str(repo/'runner/src'),str(source)]
    args += [str(repo/'runner/src'/p) for p in ['mod_operand.c','snes/interp816.c','snes/cart.c']]
    build=subprocess.run(args+['-Wl,--gc-sections','-o',str(exe)],capture_output=True,text=True)
    assert build.returncode==0,build.stdout+build.stderr
    run=subprocess.run([str(exe)],capture_output=True,text=True)
    assert run.returncode==0,run.stdout+run.stderr
    assert run.stdout.strip()=='ok'
