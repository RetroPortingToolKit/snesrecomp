"""Native entry versus real interpreter NMI; existing emulation host ABI.

The interpreter's interrupt-vector routine currently assumes native mode.
Emulation therefore pins the host's three-byte frame separately.
"""
import pathlib
import subprocess

import pytest

from _helpers import make_lorom_bank0
from test_hle_func_seam import _HARNESS, _compiler
from v2.emit_function import emit_function

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_host_interrupt_flags_and_generated_rti(tmp_path):
    cc = _compiler()
    if not cc:
        pytest.skip("C compiler required")
    # Force 8-bit A, observe the handler's P, then perform the emitted RTI.
    rom = make_lorom_bank0({0x9000: bytes.fromhex("E2 20 08 68 8D 40 00 40")})
    generated = "".join(emit_function(rom, 0, 0x9000, m, x, func_name="Handler")
                        for m in (0, 1) for x in (0, 1))
    main = r'''
#include "snes/interp816.h"
int snes_frame_counter;
int interp816_opcode_hook(uint32_t pc) { (void)pc; return 0; }
static uint8_t rd(void *p,uint32_t a) { return cpu_read8(p,a>>16,(uint16)a); }
static void wr(void *p,uint32_t a,uint8_t v) { cpu_write8(p,a>>16,(uint16)a,v); }
int main(void) {
 RecompReturn(*handlers[])(CpuState*)={Handler_M0X0,Handler_M0X1,Handler_M1X0,Handler_M1X1};
 int fails=0;
 for(int e=0;e<2;e++)for(unsigned p=0;p<256;p++) {
  if(e && (p&0x30)!=0x30)continue;
  CpuState c;memset(&c,0,sizeof c);memset(g_ram,0,sizeof g_ram);
  c.ram=g_ram;c.S=0x1f0;c.P=p;c.PB=0xab;c.emulation=e;cpu_p_to_mirrors(&c);
  unsigned pc=(p&1)?0:0xcdef;
  if(p&1)cpu_push_interrupt_frame(&c);else cpu_push_interrupt_frame_at(&c,0xabcdef);
  uint8 frame[4];memcpy(frame,g_ram+0x1ed,4);
  if(e) {
   if(c.S!=0x1ed || frame[1]!=p || frame[2]!=(uint8)pc || frame[3]!=(uint8)(pc>>8) ||
      c.P!=((p|4)&~8) || !c._flag_I || c._flag_D) { fails++;continue; }
  } else {
  Interp816 ref;memset(&ref,0,sizeof ref);ref.mem=&c;ref.read=rd;ref.write=wr;
  ref.sp=0x1f0;ref.pc=pc;ref.k=0xab;ref.e=e;interp816_setFlags(&ref,p);
  g_ram[e?0xfffa:0xffea]=0;g_ram[e?0xfffb:0xffeb]=0x90;
  ref.nmiWanted=true;interp816_runOpcode(&ref);
  if(c.S!=ref.sp || c.P!=interp816_getFlags(&ref) || !c._flag_I || c._flag_D ||
     memcmp(frame,g_ram+0x1ed,4)) {
   fprintf(stderr,"entry e=%d p=%02x live=%02x/%02x I=%d D=%d stacked=%02x/%02x\n",
      e,p,c.P,interp816_getFlags(&ref),c._flag_I,c._flag_D,frame[e?1:0],g_ram[e?0x1ee:0x1ed]);fails++;continue;
  }
  }
  handlers[((p>>5)&1)*2+((p>>4)&1)](&c);cpu_mirrors_to_p(&c);
  if((g_ram[0x40]&0x0c)!=0x04 || c.S!=0x1f0 || c.P!=p || c._flag_I!=!!(p&4) || c._flag_D!=!!(p&8)) {
   fprintf(stderr,"RTI e=%d p=%02x handler=%02x restored=%02x S=%04x\n",e,p,g_ram[0x40],c.P,c.S);fails++;
  }
 }
 return fails!=0;
}
'''
    source = tmp_path / "interrupt.c"
    source.write_text(_HARNESS.split("/* The C helpers.")[0] + generated + main, encoding="utf-8")
    exe = tmp_path / "interrupt.exe"
    build = subprocess.run([cc, "-std=c11", "-O1", "-I", str(ROOT / "runner/src"),
        str(source), str(ROOT / "runner/src/snes/interp816.c"), "-o", str(exe)],
        capture_output=True, text=True, timeout=60)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
    assert run.returncode == 0, run.stdout + run.stderr
