/* Production margin lookup: high world Y, raster phase, native identity and
 * dynamic-object precedence. No game ROM or generated code is required. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "snes/ppu.h"
#include "snes/ws_shadow.h"
#include "snes/mmx_door.h"

int snes_frame_counter;
static Ppu p;
static unsigned calls, last_x, last_y;
static WsShadowSource result = kWsShadowExact;
static WsShadowSource provider(void *context, uint32_t x, uint32_t y, uint16_t *entry) {
  assert(context == &p);
  ++calls; last_x=x; last_y=y;
  *entry=(uint16_t)(0x2000 | ((x + y * 3) & 1023));
  return result;
}
static void setup(unsigned x, unsigned y) {
  WsShadowReset(); memset(&p,0,sizeof(p));
  p.bgXsc[0]=0x51;
  WsShadowSetWorld(0,x,y);
  WsShadowSetWorldProvider(0,provider,&p,false);
  WsShadowSetRespectGameWrites(0,60);
  WsShadowFrame(&p); calls=0;
}
int main(void) {
  uint16_t x3[3][4]={{0x1813,0x5813,0x1814,0x5814},
                    {0x1815,0x5815,0x9815,0xd815},
                    {0x9814,0xd814,0x9813,0xd813}};
  uint16_t x2[3][4]={{0x0410,0x4410,0x0420,0x4420},
                    {0x0430,0x4430,0x8430,0xc430},
                    {0x8420,0xc420,0x8410,0xc410}};
  uint16_t wall[3][4]={{0x0c07,0x0c07,0x0c42,0x0c42},
                      {0x0c43,0x0c43,0x0c43,0x0c43},
                      {0x8c42,0x8c42,0x8c07,0x8c07}};
  assert(!MmxDoorBody(wall));
  assert(MmxDoorBody(x3) && MmxDoorBody(x2));
  x3[1][0] ^= 1; assert(!MmxDoorBody(x3));
  memset(x2,0,sizeof(x2)); assert(!MmxDoorBody(x2));
  setup(0x1c00,0x1300);
  assert(WsShadowTile(0,100,0x300,0,0x5000,0x1234)==0x1234 && calls==0);
  assert(WsShadowTile(0,255,0x300,0,0x5000,0x5678)==0x5678 && calls==0);
  assert(WsShadowTile(0,-7,0x300,0,0x5000,0x789a)==0x789a && calls==0);
  uint16_t got=WsShadowTile(0,256,0x308,0,0x5400,0xdead);
  assert(calls==1 && last_x==0x1d00/8 && last_y==0x1308/8);
  assert(got==(uint16_t)(0x2000|((last_x+last_y*3)&1023)));
  (void)WsShadowTile(0,-40,0x2e8,128,0x5000,0xdead);
  assert(last_x==(0x1c00-40+128)/8 && last_y==(0x1300-24)/8);
  (void)WsShadowTile(0,256,0x308,896,0x5000,0xdead);
  assert(last_x==(0x1d00-128)/8);
  setup(0,0x200); snes_frame_counter=10;
  WsShadowOnVramWrite(0x5400,0xabcd);
  assert(WsShadowTile(0,256,0x200,0,0x5400,0)==0xabcd);
  snes_frame_counter=71;
  assert(WsShadowTile(0,256,0x200,0,0x5400,0)!=0xabcd);
  setup(0,0x1300); snes_frame_counter=80;
  WsShadowOnVramWrite(0x5400,0xbcde);
  assert(WsShadowTile(0,256,0x300,0,0x5400,0)==0xbcde);
  snes_frame_counter=141;
  assert(WsShadowTile(0,256,0x300,0,0x5400,0)!=0xbcde);
  WsShadowReset();
  assert(WsShadowTile(0,256,0x200,0,0x5400,0x4567)==0x4567);
  /* Provider registration is frame scoped; other callers keep old behavior. */
  setup(0,0x200); WsShadowSetWorld(0,0,0x200); WsShadowFrame(&p); calls=0;
  (void)WsShadowTile(0,256,0x200,0,0x5400,0);
  assert(calls==0);
  /* Presentation substitutions may hide a duplicate door even inside the
   * native region or following a fresh game upload; ordinary exact entries
   * must still leave native pixels untouched. */
  setup(0,0x200);
  WsShadowSetWorldProvider(0,provider,&p,true);
  assert(WsShadowTile(0,100,0x200,0,0x5000,0x1234)==0x1234);
  result=kWsShadowOverride;
  assert(WsShadowTile(0,100,0x200,0,0x5000,0x1234)!=0x1234);
  WsShadowOnVramWrite(0x5400,0xabcd);
  assert(WsShadowTile(0,256,0x200,0,0x5400,0)!=0xabcd);
  result=kWsShadowExact;
  puts("exact world provider: high-Y, signed raster phase, native pixels, dynamic writes and lifecycle passed");
  return 0;
}
