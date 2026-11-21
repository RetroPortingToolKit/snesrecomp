/* Real PPU: complete picture replay, not a second DMA simulation. */
#define main legacy_raster_fixture_main
#include "ppu_raster_journal_test.c"
#undef main

static Ppu cpu_final, expected_reads;
static uint32_t pixels[256 * 240];
static unsigned logged_vram;
static void note_vram(uint32_t address, uint8_t value) {
  (void)address; (void)value; ++logged_vram;
}
static uint8_t read_at(uint16_t line, uint8_t reg) {
  s_snes.vPos = line; return ppu_read(&p, reg);
}
static void check_picture(void) {
  CHECK(ppu_rasterReplayReady());
  ppu_rasterRenderBegin(&p);
  CHECK(p.cgram[0] == 0x001f);
  ppu_rasterApplyLine(&p, 0);
  CHECK(p.cgram[7] == 0x1234 && p.cgram[8] == 0x0334);
  CHECK(p.oam[5] == 0x7856 && p.oam[6] == 0x9a56);
  CHECK(p.vram[3] == 0x1234 && p.vram[35] == 0x00de);
  CHECK(p.vramPointer == 67 && p.vramReadBuffer == 0x1234);
  CHECK(p.evenFrame); /* physical line0, not parity at the field's V=225 */
  ppu_renderRasterLine(&p, 1);
  ppu_rasterApplyLine(&p, 11); CHECK(p.cgram[0] == 0x03e0);
  ppu_renderRasterLine(&p, 12);
  ppu_rasterApplyLine(&p, 21); CHECK(p.cgram[0] == 0x7c00);
  ppu_renderRasterLine(&p, 22);
  ppu_rasterRenderEnd(&p);
  CHECK(memcmp(&p.inidisp, &cpu_final.inidisp, PPU_SAVESTATE_REGS_SIZE) == 0);
  CHECK(memcmp(p.cgram, cpu_final.cgram, PPU_SAVESTATE_MEM_SIZE) == 0);
  PpuRollbackResidue actual, expected;
  ppu_rb_residue_get(&p, &actual); ppu_rb_residue_get(&cpu_final, &expected);
  CHECK(memcmp(&actual, &expected, offsetof(PpuRollbackResidue, wsOamMotionLastLine)) == 0);
  CHECK(p.vramWriteCount == cpu_final.vramWriteCount && p.oamWriteCount == cpu_final.oamWriteCount);
  const uint8_t *red = (uint8_t *)pixels;
  const uint8_t *green = (uint8_t *)(pixels + 11 * 256);
  const uint8_t *blue = (uint8_t *)(pixels + 21 * 256);
  CHECK(red[2] == 255 && !red[1] && !red[0]);
  CHECK(green[1] == 255 && !green[2] && !green[0]);
  CHECK(blue[0] == 255 && !blue[1] && !blue[2]);
}

int main(void) {
  memset(&s_snes, 0, sizeof(s_snes)); memset(&p, 0, sizeof(p));
  g_snes = &s_snes; s_snes.ppu = &p; s_snes.ppuLatch = true;
  ppu_reset(&p); p.inidisp = 15;
  p.renderBuffer = (uint8_t *)pixels; p.renderPitch = 256 * 4;
  p.renderFlags = kPpuRenderFlags_NewRenderer;
  p.cgram[0] = 0x001f; p.vram[3] = 0x1234;
  p.vramPointer = 3; p.vramReadBuffer = 0x9abc; p.vramIncrement = 32;
  p.cgramPointer = 7; p.cgramSecondWrite = true; p.cgramBuffer = 0x34;
  p.oamAdr = 5; p.oamaddl = 5; p.oamSecondWrite = true; p.oamBuffer = 0x56;
  ppu_set_vram_write_log_hook(note_vram);
  s_snes.vPos = 225; ppu_rasterBeginPicture(&p);
  write_at(230, 0x22, 0x12); write_at(231, 0x04, 0x78);
  CHECK(read_at(232, 0x39) == 0xbc);
  write_at(233, 0x18, 0xde);
  CHECK(read_at(234, 0x38) == 0);
  write_at(234, 0x04, 0x9a);
  write_at(235, 0x21, 8); CHECK(read_at(235, 0x3b) == 0);
  write_at(235, 0x22, 3);
  s_snes.vPos = 0; ppu_runLineState(&p, 0);
  s_snes.vPos = 1; ppu_runLineState(&p, 1);
  CHECK(ppu_read(&p, 0x3e) & 0x40); /* live status owns the overflow latch */
  write_at(10, 0x21, 0); write_at(10, 0x22, 0xe0); write_at(10, 0x22, 3);
  write_at(20, 0x21, 0); write_at(20, 0x22, 0); write_at(20, 0x22, 0x7c);
  s_snes.vPos = 225; ppu_rasterFieldBoundary();
  /* Already executing the next field: none of this can tint the old one or
   * be lost when the old field is rendered. Keep half-written ports live. */
  write_at(230, 0x21, 0); write_at(230, 0x22, 0xff); write_at(230, 0x22, 3);
  write_at(232, 0x21, 7); write_at(232, 0x22, 0xaa);
  write_at(232, 0x02, 3); write_at(232, 0x04, 0xcc);
  write_at(232, 0x15, 0x80); write_at(232, 0x16, 0x44);
  write_at(232, 0x17, 0);
  p.hCount = 321; p.vCount = 232; p.hCountSecond = true;
  p.vCountSecond = true; p.countersLatched = true;
  cpu_final = p; expected_reads = p;
  unsigned writes = logged_vram, shadows = shadow_writes;
  PpuWlogEntry entries[128]; int lost;
  int real_writes = ppu_wlog_collect((uint32_t)snes_frame_counter, entries, 128, &lost);
  size_t size = ppu_rasterStateSize(); uint8_t *saved = malloc(size); CHECK(saved);
  JournalStream stream = {{&journal_transfer}, saved, size, true};
  CHECK(ppu_rasterStateSave(&stream.base) && ppu_rasterStateValid(saved, size));
  check_picture();
  CHECK(logged_vram == writes && shadow_writes == shadows);
  CHECK(ppu_wlog_collect((uint32_t)snes_frame_counter, entries, 128, &lost) == real_writes);
  /* Real guest read observations, including data-pointer and counter toggles,
   * must match the CPU's final state after a picture transaction. */
  for (uint8_t reg = 0x34; reg <= 0x3f; ++reg)
    CHECK(ppu_read(&p, reg) == ppu_read(&expected_reads, reg));
  PpuRollbackResidue actual, expected;
  ppu_rb_residue_get(&p, &actual); ppu_rb_residue_get(&expected_reads, &expected);
  CHECK(memcmp(&actual, &expected, offsetof(PpuRollbackResidue, wsOamMotionLastLine)) == 0);
  /* Rewind both normal device state and the existing field journal. */
  ppu_rasterReset(); p = cpu_final; stream.save = false;
  CHECK(ppu_rasterStateLoad(&stream.base)); check_picture();
  uint8_t *unaligned = malloc(size + 1); CHECK(unaligned);
  memcpy(unaligned + 1, saved, size); CHECK(ppu_rasterStateValid(unaligned + 1, size));
  CHECK(!ppu_rasterStateValid(saved, size - 1));
  uint32_t bad = 2; memcpy(unaligned + 1 + 20, &bad, sizeof(bad));
  CHECK(!ppu_rasterStateValid(unaligned + 1, size)); free(unaligned);

  /* More than the old4096 entries, but a valid bounded upload history. */
  ppu_rasterReset(); ppu_reset(&p); s_snes.vPos = 225; ppu_rasterBeginPicture(&p);
  for (unsigned i = 0; i < 6000; ++i) ppu_write(&p, 0x18, (uint8_t)i);
  s_snes.vPos = 225; ppu_rasterFieldBoundary();
  CHECK(ppu_rasterReplayReady()); ppu_rasterRenderBegin(&p);
  ppu_rasterApplyLine(&p, 0); CHECK(p.vram[5999] == (5999 & 255)); ppu_rasterRenderEnd(&p);
  /* Unsupported non-hardware bursts fail explicitly instead of truncating
   * a supposedly complete field or emitting a restorable incomplete state. */
  ppu_rasterReset(); s_snes.vPos = 225; ppu_rasterBeginPicture(&p);
  for (unsigned i = 0; i < 65537; ++i) ppu_write(&p, 0x18, (uint8_t)i);
  CHECK(!ppu_rasterReplayReady()); stream.save = true;
  CHECK(!ppu_rasterStateSave(&stream.base));
  free(saved);
  puts("PPU picture journal: PASS colors/pixels, VRAM/OAM/CGRAM read-write latches, final CPU reads, no duplicate writes, state restore and capacity gates");
  return 0;
}
