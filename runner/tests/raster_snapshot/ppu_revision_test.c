#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/ws_shadow.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Device-port tests never render. Trap an unexpected dependency instead of
 * supplying a fake renderer that could accidentally make a test pass. */
Snes *g_snes;
unsigned char g_snesrecomp_last_hdmaen;
int snes_frame_counter;
void ppu_draw_whole_line_legacy(Ppu *p,int line) { (void)p;(void)line;abort(); }
bool WsShadowLayerActive(int layer) { (void)layer;abort(); }
uint16_t WsShadowTile(int layer,int x,uint32_t y,uint16_t scroll,
                     uint16_t address,uint16_t tile) {
  (void)layer;(void)x;(void)y;(void)scroll;(void)address;(void)tile;abort();
}
uint32_t WsShadowWorldX(int layer) { (void)layer;abort(); }
uint32_t WsShadowPresentWorldY(int layer,int x) { (void)layer;(void)x;abort(); }
uint32_t WsShadowScrollY(int layer) { (void)layer;abort(); }

/* The optional shadow observer is outside this test's device contract. */
void WsShadowOnVramWrite(uint16_t address, uint16_t value) {
  (void)address; (void)value;
}
int main(void) {
  Ppu *p=ppu_init();assert(p);ppu_reset(p);
  ppu_write(p,0x15,0x80);ppu_write(p,0x16,0);ppu_write(p,0x17,0);
  ppu_write(p,0x18,0xaa);ppu_write(p,0x19,0xbb);
  assert(p->vram[0]==0xbbaa && p->vramPointer==1);
#if SNESRECOMP_PPU_CAPTURE_REVISION
  assert(p->vramCaptureRevision==2);
#endif
  ppu_write(p,0x00,15);ppu_write(p,0x16,0);ppu_write(p,0x17,0);
  assert(ppu_read(p,0x39)==0xaa && ppu_read(p,0x3a)==0xbb);
#if SNESRECOMP_PPU_CAPTURE_REVISION
  assert(p->vramCaptureRevision==2);
  p->vramCaptureRevision=UINT32_MAX;
  ppu_write(p,0x18,0xcc);assert(p->vramCaptureRevision==0);
#endif
  ppu_reset(p);assert(p->vram[0]==0);
#if SNESRECOMP_PPU_CAPTURE_REVISION
  assert(p->vramCaptureRevision==0);
#endif
  ppu_free(p);puts("PPU VRAM writes, reads, reset and capture revision: passed");
}
