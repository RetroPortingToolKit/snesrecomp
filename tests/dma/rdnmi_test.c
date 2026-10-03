/* Reuse the HDMA harness's peripheral stubs; snes.c and dma.c are real.
 * These checks need no guest image, PPU renderer, or audio implementation. */
#define main hdma_harness_main
#include "hdma_timing_test.c"
#undef main
#include <assert.h>

typedef struct Buffer {
  SaveLoadInfo base;
  unsigned char bytes[0x22000];
  size_t pos;
  bool reading;
} Buffer;
static void transfer(SaveLoadInfo *base,void *data,size_t size) {
  Buffer *b=(Buffer *)base;
  assert(b->pos+size<=sizeof(b->bytes));
  if(b->reading) memcpy(data,b->bytes+b->pos,size);
  else memcpy(b->bytes+b->pos,data,size);
  b->pos+=size;
}
static void vblank(Snes *s) {
  s->hPos=1360;s->vPos=224;
  snes_advance_master_cycles(s,8);
  assert(s->vPos==225 && s->inVblank);
}
int main(void) {
  Cpu cpu={0};Dma dma={0};Snes s={0},other={0};
  s.cpu=&cpu;s.dma=&dma;s.ram=ram;s.hdmaBeamOff=1;
  assert(snes_readReg(&s,0x4210)==2);
  for(unsigned enabled=0;enabled<2;++enabled) {
    s.nmiEnabled=enabled;vblank(&s);
    assert(!s.inNmi); // latching does not claim an interrupt was serviced
    assert(snes_readReg(&other,0x4210)==2); // no process-global latch
    assert(snes_readReg(&s,0x4210)==0x82);
    assert(snes_readReg(&s,0x4210)==2);
  }
  vblank(&s);s.hPos=1360;s.vPos=261;snes_advance_master_cycles(&s,8);
  assert(snes_readReg(&s,0x4210)==2); // unread flag expires next field
  s.inNmi=true;
  assert(snes_readReg(&s,0x4210)==0x82 && snes_readReg(&s,0x4210)==2);
  static Buffer b;
  b.base.func=transfer;
  size_t legacy_size=0;
  for(unsigned version=9;version<=10;++version) {
    for(unsigned pending=0;pending<2;++pending) {
      snes_saveload_set_version(version);s.rdnmiPending=pending;
      b.pos=0;b.reading=false;snes_saveload(&s,&b.base);
      size_t saved=b.pos;
      if(version==9) legacy_size=saved;
      else assert(saved==legacy_size+1);
      s.rdnmiPending=!pending;b.pos=0;b.reading=true;
      snes_saveload(&s,&b.base);
      assert(b.pos==saved);
      assert(s.rdnmiPending==(version>=10 && pending));
    }
  }
  s.rdnmiPending=true;snes_reset(&s,false);assert(!s.rdnmiPending);
  puts("RDNMI: disabled/enabled NMI, read-clear, field expiry, instance isolation, host compatibility, v9/v10 snapshots and reset PASS");
  return 0;
}
