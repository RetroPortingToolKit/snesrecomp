#include "framedump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "common_rtl.h"
#include "snes/ppu.h"

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

FrameDumpCallback g_framedump_callback;

static char g_framedump_dir[512];
static uint32_t g_framedump_start;
static uint32_t g_framedump_end = UINT32_MAX;
static int g_framedump_pixels;

static void put_u32(uint8_t *p, uint32_t value) {
  for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

void FrameDump_Present(uint32_t frame, const uint8_t *bgra, uint32_t pitch,
                       uint32_t width, uint32_t height) {
  if (!g_framedump_pixels || !bgra || !width || !height || width > 16384 ||
      height > 16384 || pitch < width * 4 ||
      frame < g_framedump_start || frame > g_framedump_end) return;
  char path[768];
  snprintf(path, sizeof(path), "%s/frame_%06u.bmp", g_framedump_dir, frame);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  uint8_t header[54] = {'B', 'M'};
  const uint32_t size = width * height * 4;
  put_u32(header + 2, 54 + size);
  put_u32(header + 10, 54);
  put_u32(header + 14, 40);
  put_u32(header + 18, width);
  put_u32(header + 22, 0u - height);
  header[26] = 1;
  header[28] = 32;
  put_u32(header + 34, size);
  fwrite(header, 1, sizeof(header), f);
  for (uint32_t y = 0; y < height; ++y)
    fwrite(bgra + (size_t)y * pitch, 4, width, f);
  fclose(f);
  /* Device resources at the same completed presentation boundary as the BMP.
   * Opt-in and finite; never pauses the guest or restores a captured state. */
  const char *video = getenv("SNESRECOMP_FRAMEDUMP_VIDEO");
  if (g_ppu && video && strcmp(video, "1") == 0) {
    snprintf(path, sizeof(path), "%s/frame_%06u_vram.bin", g_framedump_dir, frame);
    f = fopen(path, "wb");
    if (f) { fwrite(g_ppu->vram, 1, sizeof(g_ppu->vram), f); fclose(f); }
    snprintf(path, sizeof(path), "%s/frame_%06u_cgram.bin", g_framedump_dir, frame);
    f = fopen(path, "wb");
    if (f) { fwrite(g_ppu->cgram, 1, sizeof(g_ppu->cgram), f); fclose(f); }
    snprintf(path, sizeof(path), "%s/frame_%06u_ppu.json", g_framedump_dir, frame);
    f = fopen(path, "w");
    if (f) {
      fprintf(f, "{\"frame\":%u,\"bgmode\":%u,\"bg_tile_address\":%u,"
                 "\"bg_maps\":[%u,%u,%u,%u],\"hscroll\":[%u,%u,%u,%u],"
                 "\"vscroll\":[%u,%u,%u,%u],\"screen_enabled\":[%u,%u],"
                 "\"screen_windowed\":[%u,%u],\"cgadsub\":%u,\"cgwsel\":%u,"
                 "\"fixed_color\":%u,\"windowsel\":%u,"
                 "\"window_positions\":[%u,%u,%u,%u]}\n",
              frame,g_ppu->bgmode,g_ppu->bgTileAdr,
              g_ppu->bgXsc[0],g_ppu->bgXsc[1],g_ppu->bgXsc[2],g_ppu->bgXsc[3],
              g_ppu->hScroll[0],g_ppu->hScroll[1],g_ppu->hScroll[2],g_ppu->hScroll[3],
              g_ppu->vScroll[0],g_ppu->vScroll[1],g_ppu->vScroll[2],g_ppu->vScroll[3],
              g_ppu->screenEnabled[0],g_ppu->screenEnabled[1],
              g_ppu->screenWindowed[0],g_ppu->screenWindowed[1],
              g_ppu->cgadsub,g_ppu->cgwsel,g_ppu->fixedColor,g_ppu->windowsel,
              g_ppu->window1left,g_ppu->window1right,
              g_ppu->window2left,g_ppu->window2right);
      fclose(f);
    }
  }
}

// --- CRC32 (standard polynomial) ---
static uint32_t s_crc32_table[256];
static int s_crc32_init = 0;

static void crc32_init_table(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int j = 0; j < 8; j++)
      c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
    s_crc32_table[i] = c;
  }
  s_crc32_init = 1;
}

static uint32_t crc32(const uint8_t *buf, size_t len) {
  if (!s_crc32_init) crc32_init_table();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++)
    c = s_crc32_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// Game-agnostic per-frame metadata. Per-game offline tools decode the
// accompanying .bin dump for game-specific fields.
static void write_json(const char *path, uint32_t frame, const uint8_t *wram) {
  uint32_t crc = crc32(wram, 0x20000);
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
    "{\n"
    "  \"frame\": %u,\n"
    "  \"wram_size\": %u,\n"
    "  \"crc32_wram\": \"0x%08X\"\n"
    "}\n",
    frame, 0x20000u, crc);
  fclose(f);
}

static void write_bin(const char *path, const uint8_t *wram) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite(wram, 1, 0x20000, f);
  fclose(f);
}

static void framedump_callback(uint32_t frame, const uint8_t *wram) {
  if (!wram) return;
  if (frame < g_framedump_start || frame > g_framedump_end) return;
  char path[768];
  snprintf(path, sizeof(path), "%s/frame_%06u.json", g_framedump_dir, frame);
  write_json(path, frame, wram);
  snprintf(path, sizeof(path), "%s/frame_%06u_wram.bin", g_framedump_dir, frame);
  write_bin(path, wram);
}

void FrameDump_Init(const char *dir) {
  const char *pixels = getenv("SNESRECOMP_FRAMEDUMP_PIXELS");
  g_framedump_pixels = pixels && strcmp(pixels, "1") == 0;
  strncpy(g_framedump_dir, dir, sizeof(g_framedump_dir) - 1);
  const char *start = getenv("SNESRECOMP_FRAMEDUMP_START");
  const char *end = getenv("SNESRECOMP_FRAMEDUMP_END");
  g_framedump_start = start && *start ? (uint32_t)strtoul(start, NULL, 0) : 0;
  g_framedump_end = end && *end ? (uint32_t)strtoul(end, NULL, 0) : UINT32_MAX;
  MKDIR(dir);
  g_framedump_callback = framedump_callback;
  fprintf(stderr, "framedump: writing to '%s' frames %u..%u\n", dir,
          g_framedump_start, g_framedump_end);
}
