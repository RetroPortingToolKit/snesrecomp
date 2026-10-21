/* ROM-free test of the raster journal (ppu.c): field cut by the beam, every
 * display register journaled, render restores the completed field and
 * RenderEnd puts back what the guest wrote since. Modelled on Doom's frame:
 * vblank write, a split at line 22, a split at 199, and a CPU half that runs
 * past V=225 into the next field. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;
uint16_t WsShadowTile(int layer, int x, uint32_t y, uint16_t tile) {
  (void)layer; (void)x; (void)y; return tile;
}
bool WsShadowLayerActive(int layer) { (void)layer; return false; }
uint32_t WsShadowWorldX(int layer) { (void)layer; return 0; }
uint32_t WsShadowPresentWorldY(int layer, int x) { (void)layer; (void)x; return 0; }
uint32_t WsShadowScrollY(int layer) { (void)layer; return 0; }
void WsShadowOnVramWrite(uint16_t address, uint16_t value) { (void)address; (void)value; }

#define CHECK(test) do { if (!(test)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)

static Snes s_snes;
static Ppu p;

static void write_at(uint16_t line, uint8_t reg, uint8_t val) {
  s_snes.vPos = line;
  ppu_write(&p, reg, val);
}

int main(void) {
  memset(&s_snes, 0, sizeof(s_snes));
  memset(&p, 0, sizeof(p));
  ppu_reset(&p);
  g_snes = &s_snes;
  s_snes.ppu = &p;

  /* Unarmed: writes are not journaled and a render leaves state alone. */
  write_at(100, 0x26, 0x33);
  ppu_rasterRenderBegin(&p);
  ppu_rasterApplyLine(&p, 223);
  ppu_rasterRenderEnd(&p);
  CHECK(p.window1left == 0x33);

  /* Field A starts here (the host arms at its frame start, beam at V=225). */
  write_at(225, 0x00, 0x80);
  write_at(225, 0x26, 0xFF);
  ppu_rasterBegin(&p);
  write_at(230, 0x00, 0x0F);        /* vblank: before line 0 of field A */
  write_at(22, 0x26, 0x14);         /* top split: window, not on the old list */
  write_at(22, 0x27, 0xEB);
  write_at(199, 0x00, 0x80);        /* bottom split */
  write_at(199, 0x26, 0xFF);
  write_at(150, 0x18, 0x55);        /* VMDATA: an upload, never replayed */
  /* The bottom split's DMA carries the CPU across V=225 ... */
  s_snes.vPos = 225;
  ppu_rasterFieldBoundary();
  /* ... and what it writes after that belongs to field B. */
  write_at(10, 0x26, 0x20);
  write_at(12, 0x02, 0x40);         /* OAMADDL moves the guest's OAM port */
  p.oamAdr = 0x41;                  /* ... which a following $2104 advanced */

  /* Render field A. */
  ppu_rasterRenderBegin(&p);
  CHECK(p.inidisp == 0x80 && p.window1left == 0xFF);   /* field A's start */
  ppu_rasterApplyLine(&p, 0);
  CHECK(p.inidisp == 0x0F && p.window1left == 0xFF);   /* vblank write */
  ppu_rasterApplyLine(&p, 21);
  CHECK(p.window1left == 0xFF);
  ppu_rasterApplyLine(&p, 22);
  CHECK(p.window1left == 0x14 && p.window1right == 0xEB);
  ppu_rasterApplyLine(&p, 198);
  CHECK(p.inidisp == 0x0F);
  ppu_rasterApplyLine(&p, 199);
  CHECK(p.inidisp == 0x80 && p.window1left == 0xFF);
  ppu_rasterApplyLine(&p, 224);
  /* Field B's writes are not part of field A's picture. */
  CHECK(p.window1left == 0xFF);
  ppu_rasterRenderEnd(&p);
  /* The guest resumes with the register file it last wrote ... */
  CHECK(p.window1left == 0x20 && p.oamaddl == 0x40);
  /* ... and its OAM port where its own writes left it. */
  CHECK(p.oamAdr == 0x41);

  /* Writes made during a render (replay, render-walk HDMA) are not new
   * history: field B must hold only its two entries, so rendering it after
   * the next boundary starts from field B's baseline. */
  s_snes.vPos = 225;
  ppu_rasterFieldBoundary();
  ppu_rasterRenderBegin(&p);
  CHECK(p.window1left == 0xFF);      /* field B started at field A's end */
  ppu_rasterApplyLine(&p, 10);
  CHECK(p.window1left == 0x20);
  ppu_rasterRenderEnd(&p);

  /* A machine reset disarms. */
  ppu_rasterReset();
  write_at(50, 0x26, 0x77);
  ppu_rasterRenderBegin(&p);
  CHECK(p.window1left == 0x77);
  ppu_rasterRenderEnd(&p);

  printf("ppu_raster_journal_test: ok\n");
  return 0;
}
