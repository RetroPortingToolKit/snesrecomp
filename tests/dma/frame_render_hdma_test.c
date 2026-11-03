#define main hdma_harness_main
#include "hdma_timing_test.c"
#undef main
#include <assert.h>
#include "beam_frame_driver.h"
Snes *g_snes;
static uint8_t line_values[225];
void ppu_runLine(Ppu *p,int line) {(void)p;line_values[line]=ppu_regs[0x26];}
void ppu_rasterRenderBegin(Ppu *p) {(void)p;}
void ppu_rasterRenderEnd(Ppu *p) {(void)p;}
void ppu_rasterApplyLine(Ppu *p,int line) {(void)p;(void)line;}
int ppu_rasterTakeHdmaen(uint8_t *out) {(void)out;return 0;}

int main(void) {
  Snes snes={0};Ppu p={0};g_snes=&snes;g_ppu=&p;
  snes.ram=ram;snes.ppu=&p;snes.dma=dma_init(&snes);
  for(unsigned repeat=0;repeat<2;repeat++) {
    dma_reset(snes.dma);memset(ram,0,sizeof ram);memset(ppu_regs,0,sizeof ppu_regs);
    const uint8_t every[]={0x83,10,20,30,0};
    const uint8_t hold[]={3,10,1,20,0};
    memcpy(ram+0x100,repeat?every:hold,5);
    dma_write(snes.dma,0x4300,0);dma_write(snes.dma,0x4301,0x26);
    dma_write(snes.dma,0x4302,0);dma_write(snes.dma,0x4303,1);
    dma_write(snes.dma,0x4304,0x7e);dma_startDma(snes.dma,1,true);
    snes_beam_frame_driver_draw_ppu_frame();
    assert(line_values[1]==10);
    assert(line_values[2]==(repeat?20:10));
    assert(line_values[3]==(repeat?30:10));
    assert(line_values[4]==(repeat?30:20));
    assert(snes.dma->channel[0].terminated);
  }
  dma_free(snes.dma);puts("frame HDMA: pre-render slot consumed once, hold/repeat boundaries PASS");return 0;
}
