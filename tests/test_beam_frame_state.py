"""Execute the production whole-program state hooks without a game ROM."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT=Path(__file__).resolve().parents[1]

def compiler():
    cc=shutil.which('gcc') or shutil.which('cc')
    if not cc:pytest.skip('C compiler required')
    return cc

def execute(cc,source,exe):
    result=subprocess.run([cc,'-std=c11','-O1','-Irunner/src','-Irunner/src/snes',str(source),'-o',str(exe)],cwd=ROOT,capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    result=subprocess.run([str(exe)],capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr

def test_beam_file_execution_context(tmp_path):
    execute(compiler(),ROOT/'tests/frame_driver/beam_state_test.c',tmp_path/'beam-state.exe')

def function(source,name):
    start=source.rfind('\n',0,source.index(name))+1
    at=source.index('{',start);depth=1;end=at+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

def test_bridge_persisted_context_validation(tmp_path):
    source=(ROOT/'runner/src/snes/interp_bridge.c').read_text(encoding='utf-8')
    dynamic=next(line for line in source.splitlines() if line.startswith('typedef struct BridgeDynamicValue'))
    start=source.rfind('typedef struct {',0,source.index('} InterpBridgeRbState;'))
    end=source.index('#define INTERP_BRIDGE_RB_MAGIC',start)
    types=source[start:end]
    body=function(source,'bool interp_bridge_rb_state_valid(')
    text='''#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
'''+dynamic+'\n'+types+'\n#define INTERP_BRIDGE_RB_MAGIC 0x49425253u\n'+body+'''
int main(void) {
  InterpBridgeRbState s={0};s.magic=INTERP_BRIDGE_RB_MAGIC;s.lle_resume_pc24=0xc04e43;
  assert(interp_bridge_rb_state_valid(&s,sizeof s));
  assert(!interp_bridge_rb_state_valid(NULL,sizeof s));
  for(size_t n=0;n<sizeof s;n++)assert(!interp_bridge_rb_state_valid(&s,n));
  s.magic=0;assert(!interp_bridge_rb_state_valid(&s,sizeof s));s.magic=INTERP_BRIDGE_RB_MAGIC;
  s.bus_timing_active=2;assert(!interp_bridge_rb_state_valid(&s,sizeof s));s.bus_timing_active=0;
  s.lle_wai_yield=2;assert(!interp_bridge_rb_state_valid(&s,sizeof s));s.lle_wai_yield=0;
  s.lle_resume_pc24=0x1000000;assert(!interp_bridge_rb_state_valid(&s,sizeof s));s.lle_resume_pc24=0xc04e43;
  s.apu_pending_master=(uint64_t)UINT32_MAX+1;assert(!interp_bridge_rb_state_valid(&s,sizeof s));s.apu_pending_master=1234;
  s.dynamic_values[63].valid=2;assert(!interp_bridge_rb_state_valid(&s,sizeof s));s.dynamic_values[63].valid=1;
  s.dynamic_values[63].address=0x1000000;assert(!interp_bridge_rb_state_valid(&s,sizeof s));s.dynamic_values[63].address=0x7e1234;
  assert(interp_bridge_rb_state_valid(&s,sizeof s));return 0;
}
'''
    path=tmp_path/'bridge-check.c';path.write_text(text)
    execute(compiler(),path,tmp_path/'bridge-check.exe')


def test_complete_execution_chunk_preflight(tmp_path):
    source=(ROOT/'runner/src/common_rtl.c').read_text(encoding='utf-8')
    types=source[source.index('#define RTL_RB_RESIDUE_MAGIC'):source.index('size_t RtlRollbackSnapshotBound(')]
    body=function(source,'size_t RtlExecutionStateSize(')+function(source,'bool RtlExecutionStateValid(')
    text='''#include <assert.h>
#include <string.h>
#include "common_rtl.h"
#include "cpu_state.h"
#include "apu_frame_clock.h"
#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/ppu.h"
static Snes machine;
Snes *g_snes=&machine;
size_t interp_bridge_rb_state_size(void) {return 16;}
bool interp_bridge_rb_state_valid(const void *p,size_t n) {
  return n==16 && ((const uint8_t *)p)[0]==17;
}
'''+types+body+'''
int main(void) {
  Cart cart={0};machine.cart=&cart;
  uint8_t bytes[sizeof(uint32_t)+sizeof(RtlRollbackResidue)+sizeof(uint64_t)]={0};
  uint32_t length=sizeof(RtlRollbackResidue);memcpy(bytes,&length,sizeof length);
  RtlRollbackResidue state={0};state.magic=RTL_RB_RESIDUE_MAGIC;
  state.version=RTL_RB_RESIDUE_VERSION;state.interp[0]=17;state.ppu_rb.vramIncrement=1;
  size_t size=RtlExecutionStateSize();assert(size==sizeof bytes-8);
  memcpy(bytes+4,&state,sizeof state);
  assert(RtlExecutionStateValid(bytes,size));
  assert(!RtlExecutionStateValid(NULL,size));
  for(size_t n=0;n<size;n++)assert(!RtlExecutionStateValid(bytes,n));
  for(unsigned fault=0;fault<12;fault++) {
    RtlRollbackResidue bad=state;
    switch(fault) {
      case 0:bad.magic=0;break;
      case 1:bad.version++;break;
      case 2:bad.cpu.ram=(uint8_t *)1;break;
      case 3:bad.cpu.host_return_valid=1;break;
      case 4:bad.apu_port.qTail=APU_PORT_QUEUE_LEN+1;break;
      case 5:((uint8_t *)&bad.apu_port)[offsetof(ApuPortSched,timeValid)]=2;break;
      case 6:bad.interp[0]=0;break;
      case 7:bad.cpu.m_flag=2;break;
      case 8:bad.memsel=2;break;
      case 9:bad.ppu_rb.vramIncrement=0;break;
      case 10:bad.ppu_rb.vramRemapMode=4;break;
      case 11:bad.ppu_rb.cgramSecondWrite=2;break;
    }
    memcpy(bytes+4,&bad,sizeof bad);assert(!RtlExecutionStateValid(bytes,size));
  }
  memcpy(bytes+4,&state,sizeof state);
  cart.cx4=(struct Cx4 *)1;assert(RtlExecutionStateSize()==sizeof bytes);
  assert(!RtlExecutionStateValid(bytes,size));
  assert(RtlExecutionStateValid(bytes,sizeof bytes));return 0;
}
'''
    path=tmp_path/'execution-check.c';path.write_text(text,encoding='utf-8')
    execute(compiler(),path,tmp_path/'execution-check.exe')
