/* Actual CPU beam, real PPU and production BFD draw path. No DMA re-simulation. */
#define SNES_TEST_REAL_PPU 1
#define main hdma_peripheral_fixture_main
#include "hdma_timing_test.c"
#undef main
#include <stdlib.h>
#include "beam_frame_driver.h"
#define REQUIRE(test) do { if (!(test)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)
Snes *g_snes;
uint16_t WsShadowTile(int l,int x,uint32_t y,uint16_t s,uint16_t a,uint16_t t) {
  (void)l;(void)x;(void)y;(void)s;(void)a;return t;
}
bool WsShadowLayerActive(int l) {(void)l;return false;}
uint32_t WsShadowWorldX(int l) {(void)l;return 0;}
uint32_t WsShadowPresentWorldY(int l,int x) {(void)l;(void)x;return 0;}
uint32_t WsShadowScrollY(int l) {(void)l;return 0;}
void WsShadowOnVramWrite(uint16_t a,uint16_t v) {(void)a;(void)v;}
static Ppu p, final_ppu;
static Dma final_dma;
static uint32_t pixels[256*240], first_pixels[256*240];
static uint16_t colors[225];
static uint8_t windows[225];
static uint64_t charged;
static void charge_cpu(Snes *s,uint64_t clocks) {
  charged+=clocks;g_cpu.master_cycles+=clocks;
  snes_sync_master_clock(s,g_cpu.master_cycles);
}
static void execute(uint32_t clocks) {
  g_cpu.master_cycles+=clocks;snes_sync_master_clock(g_snes,g_cpu.master_cycles);
}
static void park(uint32_t clocks) {
  g_cpu.master_cycles+=clocks;snes_sync_master_clock_parked(g_snes,g_cpu.master_cycles);
}
static void edge(void) {
  execute(g_snes->hPos<1104 ? 1104-g_snes->hPos : 1364-g_snes->hPos+1104);
}
static void channel(unsigned c,unsigned mode,unsigned table,unsigned target) {
  unsigned r=0x4300+c*16;dma_write(g_snes->dma,r,mode);
  dma_write(g_snes->dma,r+1,target);dma_write(g_snes->dma,r+2,table&255);
  dma_write(g_snes->dma,r+3,table>>8);dma_write(g_snes->dma,r+4,0x7e);
}
static void observed(const Ppu *ppu,unsigned line,void *ctx) {
  (void)ctx;colors[line]=ppu->cgram[0];windows[line]=ppu->window1left;
}
static void draw(void) {
  snes_beam_frame_driver_draw_ppu_frame_observed(observed,NULL);REQUIRE(!g_fail);
  REQUIRE(colors[1]==0x03e0 && windows[1]==10);
  REQUIRE(colors[2]==0x7c00 && windows[2]==20);
  REQUIRE(colors[3]==0x7c00 && windows[3]==30);
  REQUIRE(((uint8_t *)pixels)[1]==255 && ((uint8_t *)pixels)[0]==0);
  REQUIRE(((uint8_t *)(pixels+256))[0]==255 && ((uint8_t *)(pixels+256))[1]==0);
  REQUIRE(!memcmp(&p.inidisp,&final_ppu.inidisp,PPU_SAVESTATE_REGS_SIZE));
  REQUIRE(!memcmp(p.cgram,final_ppu.cgram,PPU_SAVESTATE_MEM_SIZE));
  PpuRollbackResidue actual,expected;
  ppu_rb_residue_get(&p,&actual);ppu_rb_residue_get(&final_ppu,&expected);
  REQUIRE(!memcmp(&actual,&expected,offsetof(PpuRollbackResidue,wsOamMotionLastLine)));
  REQUIRE(!memcmp(g_snes->dma,&final_dma,sizeof(final_dma)));
}
typedef struct {SaveLoadInfo base;uint8_t *bytes;size_t size;bool save;} Stream;
static void transfer(SaveLoadInfo *base,void *data,size_t size) {
  Stream *s=(Stream *)base;REQUIRE(size==s->size);
  if(s->save)memcpy(s->bytes,data,size);else memcpy(data,s->bytes,size);
}
#ifndef SNES_TEST_PICTURE_NO_MAIN
int main(void) {
  Snes s={0};g_snes=&s;g_ppu=&p;s.ppu=&p;s.ram=ram;s.dma=dma_init(&s);
  REQUIRE(s.dma);dma_reset(s.dma);ppu_reset(&p);p.inidisp=15;p.cgram[0]=0x001f;
  p.renderBuffer=(uint8_t *)pixels;p.renderPitch=256*4;
  p.renderFlags=kPpuRenderFlags_NewRenderer;s.vPos=225;
  snes_set_ppu_beam_enabled(&s,true);snes_set_master_clock_charge_hook(charge_cpu);
  ppu_rasterBeginPicture(&p);
  const uint8_t window[]={0x83,10,20,30,0};memcpy(ram+0x100,window,sizeof window);
  /* Mode3 sends CGADD twice then both bytes to CGDATA. */
  const uint8_t palette[]={0x81,0,0,0xe0,3,0x81,0,0,0,0x7c,0};
  memcpy(ram+0x200,palette,sizeof palette);channel(0,0,0x100,0x26);channel(1,3,0x200,0x21);
  snes_writeReg(&s,0x420c,3);
  park(37*1364);REQUIRE(s.vPos==0 && s.hPos==0 && charged==0);
  edge();REQUIRE(p.cgram[0]==0x03e0 && p.window1left==10 && charged==74);
  execute(1364-s.hPos+100);REQUIRE(s.vPos==1 && s.hPos==100);
  snes_writeBBus(&s,0x21,0);snes_writeBBus(&s,0x22,0xff);snes_writeBBus(&s,0x22,3);
  REQUIRE(p.cgram[0]==0x03ff);edge();REQUIRE(p.cgram[0]==0x7c00 && p.window1left==20 && charged==148);
  edge();REQUIRE(p.window1left==30 && charged==182);
  park((225-s.vPos)*1364-s.hPos);REQUIRE(s.vPos==225 && s.hPos==0);
  REQUIRE(ppu_read(&p,0x3e)&0x40); /* native live sprite-status evaluation */
  park(5*1364);snes_writeBBus(&s,0x21,0);snes_writeBBus(&s,0x22,0xff);snes_writeBBus(&s,0x22,3);
  final_ppu=p;final_dma=*s.dma;uint64_t clock=g_cpu.master_cycles,beam=s.beamMasterLast,cost=charged;
  size_t size=ppu_rasterStateSize();uint8_t *saved=malloc(size);REQUIRE(saved);
  Stream stream={{transfer},saved,size,true};REQUIRE(ppu_rasterStateSave(&stream.base));
  draw();REQUIRE(g_cpu.master_cycles==clock && s.beamMasterLast==beam && charged==cost);
  memcpy(first_pixels,pixels,sizeof pixels);draw();REQUIRE(!memcmp(first_pixels,pixels,sizeof pixels));
  ppu_rasterReset();p=final_ppu;*s.dma=final_dma;stream.save=false;
  REQUIRE(ppu_rasterStateLoad(&stream.base));draw();REQUIRE(!memcmp(first_pixels,pixels,sizeof pixels));
  free(saved);dma_free(s.dma);snes_set_master_clock_charge_hook(NULL);
  puts("BFD picture: PASS real timed CPU/HDMA order, pixels, live status, no second DMA, final ports/memory and current-build replay");return 0;
}
#endif
