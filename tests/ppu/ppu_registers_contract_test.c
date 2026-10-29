/* The compact interface must compile and run without the Ppu device header. */
#include "snes/raster_register_plan.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  unsigned char unaligned[66];
  for (unsigned i=0;i<66;++i) unaligned[i]=(unsigned char)(i*73u+19u);
  SnesRenderRegisters r=SnesRenderRegistersLoad(unaligned+1);
  assert(sizeof(r)==64 && memcmp(&r,unaligned+1,64)==0);
  r.bgmode=1;
  r.screenEnabled[0]=7; r.screenEnabled[1]=0;
  r.screenWindowed[0]=r.screenWindowed[1]=0;
  r.windowsel=0;
  static uint16_t vram[0x8000];
  uint16_t pixels[18];
  for(unsigned i=0;i<18;++i)pixels[i]=0xa55a;
  r.bgTileAdr=1;
  r.bgXsc[0]=0;
  r.hScroll[0]=r.vScroll[0]=0;
  vram[0]=vram[1]=0x4000; /* horizontal flip */
  vram[0x1000]=0x0080;
  assert(SnesRenderRegistersMode1TileRow(&r,vram,0,0,0,16,pixels+1));
  assert(pixels[0]==0xa55a && pixels[17]==0xa55a);
  for(unsigned i=0;i<16;++i)assert(pixels[i+1]==((i&7)==7?0x8001:0));
  SnesRenderWindowPlan windows=SnesRenderRegistersWindows(&r,0,255,0,255);
  SnesRenderComposePlan plan;
  assert(SnesRenderRegistersPlanCompose(&plan,&r,&windows,-43,342,NULL,0));
  assert(plan.count==3 && plan.spans[1].main[0]==0xffff);
  assert(!SnesRenderRegistersMode1TileRow(NULL,vram,0,0,0,1,pixels));
  assert(!SnesRenderRegistersPlanCompose(&plan,NULL,&windows,0,1,NULL,0));
  puts("Compact 64-byte registers: standalone, unaligned copy, guarded tile row and compose pass");
  return 0;
}
