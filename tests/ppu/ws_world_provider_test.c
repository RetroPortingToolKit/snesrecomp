/* Production margin lookup: high world Y, raster phase, native identity and
 * dynamic-object precedence. No game ROM or generated code is required. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "snes/ppu.h"
#include "snes/ws_shadow.h"

int snes_frame_counter;
static Ppu p;
static unsigned calls, last_x, last_y;
static bool provider(void *context, uint32_t x, uint32_t y, uint16_t *entry) {
  assert(context == &p);
  ++calls; last_x=x; last_y=y;
  *entry=(uint16_t)(0x2000 | ((x + y * 3) & 1023));
  return true;
}
static void setup(unsigned x, unsigned y) {
  WsShadowReset(); memset(&p,0,sizeof(p));
  p.bgXsc[0]=0x51;
  WsShadowSetWorld(0,x,y);
  WsShadowSetWorldProvider(0,provider,&p);
  WsShadowSetRespectGameWrites(0,60);
  WsShadowFrame(&p); calls=0;
}
int main(void) {
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
  WsShadowReset();
  assert(WsShadowTile(0,256,0x200,0,0x5400,0x4567)==0x4567);
  /* Provider registration is frame scoped; other callers keep old behavior. */
  setup(0,0x200); WsShadowSetWorld(0,0,0x200); WsShadowFrame(&p); calls=0;
  (void)WsShadowTile(0,256,0x200,0,0x5400,0);
  assert(calls==0);
  puts("exact world provider: high-Y, signed raster phase, native pixels, dynamic writes and lifecycle passed");
  return 0;
}
