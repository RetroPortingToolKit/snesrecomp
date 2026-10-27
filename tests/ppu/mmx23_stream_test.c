/* Exercise the production source selector against deliberately stale VRAM.
 * No game ROM is needed: the fixture supplies a small metatile dictionary. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "snes/cart.h"
#include "snes/ppu.h"
#include "snes/ws_shadow.h"

int snes_frame_counter;
static uint8_t rom[65536], ram[0x20000];
static Ppu p;
static Snes snes;
static Cart cart;
uint8_t cart_read(Cart *unused, uint8_t bank, uint16_t address) {
  (void)unused; assert(bank == 0x0c); return rom[address];
}
#include "../../runner/src/snes/mmx23_background.c"

static void put(uint8_t *data, unsigned address, unsigned value) {
  data[address] = (uint8_t)value; data[address+1] = (uint8_t)(value>>8);
}
static void init(void) {
  WsShadowReset(); memset(&p,0,sizeof(p)); memset(ram,0,sizeof(ram));
  memset(rom,0,sizeof(rom)); snes.cart=&cart;
  p.bgmode=1; p.bgXsc[0]=0x51; p.bgXsc[1]=0x59;
  for (int i=0;i<2;i++) {
    put(ram,0x9c5+i*3,0x8000); ram[0x9c7+i*3]=0x0c;
  }
  for (unsigned i=0;i<4;i++) put(rom,0x8000+i*2,0x0c00);
  for (unsigned i=0;i<0x800;i++) p.vram[0x5000+i]=p.vram[0x5800+i]=0x0c00;
}
int main(void) {
  init(); put(ram,0x1e5d,7680); put(ram,0x1e60,4106);
  p.hScroll[0]=512; p.vScroll[0]=10;
  /* Blank sky is valid terrain. A stale outside column must not disable the
   * provider, while the repurposed-object safeguard remains on blank BG2. */
  p.vram[0x5000+27]=0xdead;
  Mmx23ConfigureWsBgMargins(&snes,&p,ram,true);
  assert(WsShadowLayerActive(0)); assert(!WsShadowLayerActive(1));
  assert(WsShadowTile(0,-40,10,512,0x501b,0xdead)==0x0c00);
  assert(WsShadowTile(0,100,10,512,0x5000,0x1234)==0x1234);
  /* A genuinely different foreground still fails native self-validation. */
  p.vram[vram_word(&kBgStreams[0],7690,4118)]=0x1234;
  p.vram[vram_word(&kBgStreams[0],7710,4154)]=0x1234;
  Mmx23ConfigureWsBgMargins(&snes,&p,ram,true);
  assert(!WsShadowLayerActive(0));

  init();
  for (unsigned block=0;block<256;block++) {
    put(ram,0xa600+block*2,block);
    for (unsigned q=0;q<4;q++) put(rom,0x8000+block*8+q*2,0x1000+block*4+q);
  }
  put(ram,0x1e9d,256); put(ram,0x1ea0,256);
  p.hScroll[1]=968; p.vScroll[1]=256;
  s_ram=ram; s_snes=&snes;
  for (unsigned y=0;y<256;y+=8) for (unsigned x=0;x<512;x+=8)
    p.vram[vram_word(&kBgStreams[1],456+x,256+y)]=world_tile(&kBgStreams[1],456+x,256+y);
  Mmx23ConfigureWsBgMargins(&snes,&p,ram,true);
  assert(WsShadowLayerActive(1));
  assert(WsShadowWorldX(1)==456 && WsShadowWorldY(1)==256);
  assert(WsShadowScrollX(1)==968 && WsShadowScrollY(1)==256);
  assert(WsShadowTile(1,-40,256,968,0x5800,0xdead)==world_tile(&kBgStreams[1],416,256));
  assert(WsShadowTile(1,256,264,968,0x5800,0xdead)==world_tile(&kBgStreams[1],712,264));
  p.bgmode=2; /* Same source maps during the arena's offset-per-tile shimmer. */
  Mmx23ConfigureWsBgMargins(&snes,&p,ram,true);
  assert(WsShadowLayerActive(1));
  assert(WsShadowTile(1,-40,256,968,0x5800,0xdead)==world_tile(&kBgStreams[1],416,256));
  p.bgmode=3;
  Mmx23ConfigureWsBgMargins(&snes,&p,ram,true);
  assert(!WsShadowLayerActive(1));
  init(); s_ram=ram; s_snes=&snes;
  const uint16_t doors[2][3][4]={
    {{0x0410,0x4410,0x0420,0x4420},{0x0430,0x4430,0x8430,0xc430},{0x8420,0xc420,0x8410,0xc410}},
    {{0x1813,0x5813,0x1814,0x5814},{0x1815,0x5815,0x9815,0xd815},{0x9814,0xd814,0x9813,0xd813}}};
  for (unsigned game=0;game<2;game++) {
    memset(s_tile_keys,0,sizeof(s_tile_keys)); memset(s_door_keys,0,sizeof(s_door_keys));
    for (unsigned row=0;row<3;row++) {
      put(ram,0x2800+row*32,row);
      for (unsigned q=0;q<4;q++) put(rom,0x8000+row*8+q*2,doors[game][row][q]);
    }
    assert(door_body(0,0) && door_body(0,16) && door_body(0,32));
  }
  init(); s_ram=ram; s_snes=&snes;
  for (unsigned block=1;block<256;block++)
    for (unsigned q=0;q<4;q++) put(rom,0x8000+block*8+q*2,0x1000+block*4+q);
  ram[0xe800+32+10]=1; ram[0xe800+32+11]=2;
  for (unsigned row=0;row<16;row++) for (unsigned col=0;col<16;col++) {
    unsigned block=1+((row*16+col)%255);
    put(ram,0x2800+512+row*32+col*2,block);
    put(ram,0x2800+1024+row*32+col*2,col<8?block:0);
  }
  put(ram,0x1e5d,0xa80); put(ram,0x1e60,0x100);
  p.hScroll[0]=0x280; p.vScroll[0]=0x100;
  for (unsigned y=0;y<224;y+=8) for (unsigned x=0;x<256;x+=8)
    p.vram[vram_word(&kBgStreams[0],0xa80+x,0x100+y)]=world_tile(&kBgStreams[0],0xa80+x,0x100+y);
  Mmx23ConfigureWsBgMarginsForRoom(&snes,&p,ram,true,0xb80);
  assert(WsShadowLayerActive(0));
  assert(world_tile(&kBgStreams[0],0xb80,0x100)==0x0c00);
  snes_frame_counter=100;
  WsShadowOnVramWrite(vram_word(&kBgStreams[0],0xb80,0x100),0xdead);
  assert(WsShadowTile(0,256,0x100,0x280,0x5000,0xdead)==world_tile(&kBgStreams[0],0xa80,0x100));
  assert(WsShadowTile(0,100,0x100,0x280,0x5000,0x1234)==0x1234);
  assert(WsShadowTile(0,-40,0x100,0x280,0x5000,0xdead)==world_tile(&kBgStreams[0],0xa58,0x100));
  Mmx23ConfigureWsBgMargins(&snes,&p,ram,true);
  assert(!s_bg1_repeat_right); /* ordinary room/load clears the extension */
  puts("X2/X3 source selection: blank terrain, object safeguard and 512px parallax alias passed");
  return 0;
}
