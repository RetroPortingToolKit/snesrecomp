/* Real PPU scanout: private actor colors, depth, windows and both renderers. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
uint16_t WsShadowTile(int layer,int x,uint32_t y,uint16_t scroll,
                     uint16_t address,uint16_t tile) {
  (void)layer;(void)x;(void)y;(void)scroll;(void)address;return tile;
}
bool WsShadowLayerActive(int layer) {(void)layer;return false;}
uint32_t WsShadowWorldX(int layer) {(void)layer;return 0;}
uint32_t WsShadowWorldY(int layer) {(void)layer;return 0;}
uint32_t WsShadowScrollX(int layer) {(void)layer;return 0;}
uint32_t WsShadowScrollY(int layer) {(void)layer;return 0;}
uint32_t WsShadowPresentWorldY(int layer,int x) {(void)layer;(void)x;return 0;}
void WsShadowOnVramWrite(uint16_t address,uint16_t value) {(void)address;(void)value;}

static Ppu p;
static uint32_t frame[256*4];
static uint16_t pixels[]={0x83e0,0x8000,0,0xfc00};
static PpuExtraObject object={30,0,4,1,4,2,0,64,1,pixels};

static void setup(unsigned mode,unsigned flags) {
  memset(&p,0,sizeof(p));ppu_reset(&p);
  p.inidisp=15;p.bgmode=mode;p.screenEnabled[0]=16;
  for(unsigned i=0;i<128;++i)p.oam[i*2]=0xf000;
  p.cgram[0]=31; /* backdrop red; opaque black must remain opaque */
  PpuSetExtraObjects(&p,&object,1);
  PpuBeginDrawing(&p,(uint8_t *)frame,256*4,flags);
  ppu_runLine(&p,0);
}
static void scanout(void) {
  for(unsigned mode=1;mode<=7;mode+=6)for(unsigned fast=0;fast<2;++fast) {
    setup(mode,fast?kPpuRenderFlags_NewRenderer:0);
    uint8_t before[PPU_SAVESTATE_MEM_SIZE];memcpy(before,p.cgram,sizeof(before));
    ppu_runLine(&p,1);
    assert(frame[30]==0x00ff00 && frame[31]==0 && frame[32]==0xff0000 && frame[33]==0x0000ff);
    assert(!memcmp(before,p.cgram,sizeof(before)));
    /* Window W1 hides OBJ at x30..31, revealing the ordinary backdrop. */
    p.windowsel=2u<<16;p.window1left=30;p.window1right=31;p.screenWindowed[0]=16;
    ppu_runLine(&p,1);assert(frame[30]==0xff0000 && frame[33]==0x0000ff);
    p.screenWindowed[0]=0;
    /* Math-enabled extra objects use the native subtract/fixed-color path. */
    object.math=1;p.cgadsub=0x90;p.fixedColor=31<<5;
    ppu_runLine(&p,1);assert(frame[30]==0 && frame[33]==0xff);
    object.math=0;p.cgadsub=0;
    PpuSetExtraObjects(&p,NULL,0);ppu_runLine(&p,1);assert(frame[30]==0xff0000);
  }
}
static void ordering_and_clipping(void) {
  uint16_t z[6]={0},color[6]={0};uint64_t order[6];memset(order,0xff,sizeof(order));
  PpuExtraObject objects[17];
  for(unsigned i=0;i<17;++i) {objects[i]=object;objects[i].x=-1;objects[i].order=17-i;}
  assert(PpuComposeExtraObjects(objects,17,0,0,6,0,z,color,order));
  assert(color[0]==0x8000 && color[1]==0 && color[2]==0xfc00);
  assert((uint32_t)order[0]==1); /* list order does not decide ties */
  /* A native object of the same slot precedes all extras anchored to it. */
  order[2]=(uint64_t)64<<32;color[2]=0;z[2]=0xe6af;
  PpuComposeExtraObjects(objects,17,0,0,6,0,z,color,order);
  assert(color[2]==0 && z[2]==0xe6af);
  memset(order,0xff,sizeof(order));memset(color,0,sizeof(color));
  PpuComposeExtraObjects(objects,17,0,0,6,65,z,color,order);
  assert(order[0]==((uint64_t)127<<32|1)); /* rotated OAM order */
  objects[0].x=INT32_MAX;objects[0].y=INT32_MIN;
  assert(!PpuComposeExtraObjects(objects,1,0,0,6,0,z,color,order));
}
static void background_depth(void) {
  for(unsigned fast=0;fast<2;++fast) {
    setup(1,fast?kPpuRenderFlags_NewRenderer:0);
    p.screenEnabled[0]=17;p.bgXsc[0]=0x10;p.cgram[1]=31<<10;
    for(unsigned i=0;i<32;++i)p.vram[0x1000+i]=0x2001;
    for(unsigned i=0;i<8;++i)p.vram[16+i]=0x00ff;
    object.priority=2;ppu_runLine(&p,1);assert(frame[30]==0xff);
    object.priority=3;ppu_runLine(&p,1);assert(frame[30]==0xff00);
  }
  object.priority=2;
}
int main(void) {
  scanout();ordering_and_clipping();background_depth();
  puts("extra OBJ: native/Mode7, legacy/fast, private colors, transparency, windows, math and stable N-object order passed");
  return 0;
}
