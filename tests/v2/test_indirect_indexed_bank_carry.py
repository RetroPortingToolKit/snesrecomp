"""Execute #240's two addressing modes against the actual interpreter.

Include loads/stores, both widths, bank wrap, high-byte crossings, and bus
read counts. Synthetic memory only; clock parity is a separate contract.
"""
import pathlib
import subprocess

import pytest

from _helpers import make_lorom_bank0
from test_hle_func_seam import _HARNESS, _compiler
from v2.emit_function import emit_function

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_indirect_indexed_execution_matches_interpreter(tmp_path):
    cc = _compiler()
    if not cc:
        pytest.skip("C compiler required")
    support = _HARNESS.split("/* The C helpers.")[0]
    support = support.replace("uint8 g_ram[0x20000];", "uint8 g_ram[0x1000000];\nstatic unsigned ptr_reads; static uint16 ptr_at;")
    support = support.replace("(void)cpu; (void)bank; return g_ram[addr];",
        "if (!bank && (addr==ptr_at || addr==(uint16)(ptr_at+1))) ptr_reads++; "
        "return cpu->open_bus=g_ram[((uint32)bank<<16)|addr];")
    support = support.replace("(void)cpu; (void)bank; g_ram[addr] = v;",
        "g_ram[((uint32)bank<<16)|addr]=v; cpu->open_bus=v;")
    emitted = ""
    calls = []
    for stack in (0, 1):
        for store in (0, 1):
            opcode = (0x93 if stack else 0x91) if store else (0xb3 if stack else 0xb1)
            code = bytes([opcode, 0x10 if stack else 0, 0x60])
            for m in (0, 1):
                for x in (0, 1):
                    name = f"Case{stack}{store}{m}{x}"
                    emitted += emit_function(make_lorom_bank0({0x8000: code}), 0, 0x8000, m, x, func_name=name)
                    calls.append(f"fails += check({name}_M{m}X{x}, {opcode}, {stack}, {store}, {m}, {x});")
    main = r'''
#include "snes/interp816.h"
int snes_frame_counter;
int interp816_opcode_hook(uint32_t pc) { (void)pc; return 0; }
static uint8_t rd(void *p,uint32_t a) { return cpu_read8(p,a>>16,(uint16)a); }
static void wr(void *p,uint32_t a,uint8_t v) { cpu_write8(p,a>>16,(uint16)a,v); }
static void prepare(CpuState *c,int opcode,int stack,int m,int x,int db,int ptr,int y) {
 memset(c,0,sizeof *c); memset(g_ram,0,sizeof g_ram);
 c->ram=g_ram;c->A=0x1234;c->S=0x1ff;c->D=0x100;c->DB=db;c->Y=y;
 c->P=(m?0x20:0)|(x?0x10:0);cpu_p_to_mirrors(c);
 cpu_push_jsr_return_frame(c);
 g_ram[0x8000]=opcode;g_ram[0x8001]=stack?0x10:0;g_ram[0x8002]=0x60;
 ptr_at=stack?(uint16)(c->S+0x10):c->D;
 g_ram[ptr_at]=ptr;g_ram[(uint16)(ptr_at+1)]=ptr>>8;
 uint32 wrong=((uint32)db<<16)|(uint16)(ptr+y);
 uint32 correct=(((uint32)db<<16)+ptr+y)&0xffffff;
 g_ram[wrong]=0x11;g_ram[(wrong+1)&0xffffff]=0x22;
 g_ram[correct]=0xab;g_ram[(correct+1)&0xffffff]=0x4c;
 g_other_exits=g_tc_valid=g_recomp_stack_top=ptr_reads=0;
}
static int check(RecompReturn(*fn)(CpuState*),int opcode,int stack,int store,int m,int x) {
 int fails=0;
 const int pointers[]={0xfffc,0xfffd,0xfffe,0xffff,0x1234};
 for(int dbi=0;dbi<2;dbi++)for(unsigned pi=0;pi<sizeof pointers/sizeof *pointers;pi++) {
  int db=dbi?0xff:0x7e,ptr=pointers[pi],y=x?2:0x102;
  uint32 address=(((uint32)db<<16)+ptr+y)&0xffffff;
  uint32 wrong=((uint32)db<<16)|(uint16)(ptr+y);
  CpuState native,memory;prepare(&native,opcode,stack,m,x,db,ptr,y);
  fn(&native);cpu_mirrors_to_p(&native);
  unsigned reads=ptr_reads,exits=g_other_exits;
  uint8 data[]={g_ram[address],g_ram[(address+1)&0xffffff],g_ram[wrong]};
  prepare(&memory,opcode,stack,m,x,db,ptr,y);
  Interp816 ref;memset(&ref,0,sizeof ref);ref.mem=&memory;ref.read=rd;ref.write=wr;
  ref.a=memory.A;ref.x=memory.X;ref.y=memory.Y;ref.sp=memory.S;ref.dp=memory.D;ref.db=db;ref.pc=0x8000;
  interp816_setFlags(&ref,memory.P);ref.e=false;
  unsigned steps=0;while(ref.pc!=1 && steps++<8)interp816_runOpcode(&ref);
  if(native.A!=ref.a || native.P!=interp816_getFlags(&ref) || native.S!=ref.sp ||
     ref.pc!=1 || exits || reads!=2 || ptr_reads!=2 ||
     data[0]!=g_ram[address] || data[1]!=g_ram[(address+1)&0xffffff] || data[2]!=g_ram[wrong]) {
   fprintf(stderr,"op=%02x m=%d x=%d db=%02x ptr=%04x: A=%04x/%04x P=%02x/%02x reads=%u/%u data=%02x/%02x\n",
      opcode,m,x,db,ptr,native.A,ref.a,native.P,interp816_getFlags(&ref),reads,ptr_reads,data[0],g_ram[address]);fails++;
  }
 }
 return fails;
}
int main(void) { int fails=0;
CALLS
return fails!=0; }
'''.replace("CALLS", "\n".join(calls))
    source = tmp_path / "indirect.c"
    source.write_text(support + emitted + main, encoding="utf-8")
    exe = tmp_path / "indirect.exe"
    build = subprocess.run([cc, "-std=c11", "-O1", "-I", str(ROOT / "runner/src"),
        str(source), str(ROOT / "runner/src/snes/interp816.c"), "-o", str(exe)],
        capture_output=True, text=True, timeout=60)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, run.stdout + run.stderr
