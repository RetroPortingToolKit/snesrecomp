#ifndef SNESRECOMP_RASTER_INSPECTOR_PANEL_H
#define SNESRECOMP_RASTER_INSPECTOR_PANEL_H

#include "snes/raster_inspection.h"

/* Use the desktop's existing modal overlay compositor, font and swatches.
 * The cursor is an index into frozen captures, never into gameplay history. */
enum { kRasterPanelWidth = 512, kRasterPanelHeight = 448 };

typedef struct RasterInspectorPanel {
  uint32_t frames[kSnesInspectionFrames];
  unsigned count, cursor, row, sprite, mask;
  bool open, dirty;
  uint32_t pixels[kRasterPanelWidth * kRasterPanelHeight];
  uint32_t field[256 * kSnesInspectionLines];
} RasterInspectorPanel;

static bool RasterPanelOpen(RasterInspectorPanel *panel) {
  panel->count = debug_server_raster_frames(panel->frames, kSnesInspectionFrames);
  if (!panel->count) return false;
  panel->cursor = panel->count - 1;
  panel->row = 112;
  panel->sprite = 0;
  panel->mask = 0x1f;
  panel->open = panel->dirty = true;
  return true;
}

static void RasterPanelHandleKey(RasterInspectorPanel *panel, int key, int repeat) {
  switch (key) {
  case SDLK_ESCAPE: panel->open = false; return;
  case SDLK_LEFT: if (panel->cursor > 0) --panel->cursor; break;
  case SDLK_RIGHT: if (panel->cursor + 1 < panel->count) ++panel->cursor; break;
  case SDLK_HOME: panel->cursor = 0; break;
  case SDLK_END: panel->cursor = panel->count - 1; break;
  case SDLK_UP: if (panel->row > 1) --panel->row; break;
  case SDLK_DOWN: if (panel->row < kSnesInspectionLines) ++panel->row; break;
  case SDLK_PAGEUP: panel->sprite = (panel->sprite + 127) % 128; break;
  case SDLK_PAGEDOWN: panel->sprite = (panel->sprite + 1) % 128; break;
  default:
    if (repeat || key < SDLK_1 || key > SDLK_5) return;
    panel->mask ^= 1u << (key - SDLK_1);
    break;
  }
  panel->dirty = true;
}

static void RasterPanelText(RasterInspectorPanel *panel, int x, int y, const char *text, int scale) {
  snes_ovl_draw_text(panel->pixels, kRasterPanelWidth, kRasterPanelHeight,
                     x, y, text, 0xffe2e5eb, scale);
}

static uint32_t RasterPanelColor(uint16_t color) {
  unsigned r = (color & 31) * 255 / 31;
  unsigned g = ((color >> 5) & 31) * 255 / 31;
  unsigned b = ((color >> 10) & 31) * 255 / 31;
  return 0xff000000u | (r << 16) | (g << 8) | b;
}

static void RasterPanelDraw(RasterInspectorPanel *panel) {
  const uint32_t frame = panel->frames[panel->cursor];
  SnesRasterInspectionView view;
  bool available = debug_server_raster_view(
      frame, panel->row, panel->mask, panel->sprite, &view, panel->field);
  snes_ovl_fill_rect(panel->pixels, kRasterPanelWidth, kRasterPanelHeight,
                     0, 0, kRasterPanelWidth, kRasterPanelHeight, 0xff151b26);
  RasterPanelText(panel, 18, 14, "RASTER INSPECTOR", 2);
  char caption[64];
  snprintf(caption, sizeof(caption), "PAUSED  FRAME %u (%u/%u)  LINE %u",
           frame, panel->cursor + 1, panel->count, panel->row);
  RasterPanelText(panel, 18, 44, caption, 1);
  if (available) {
    for (unsigned y = 0; y < kSnesInspectionLines; ++y) {
      uint32_t *dst = panel->pixels + (70 + y) * kRasterPanelWidth + 18;
      const uint32_t *src = panel->field + y * 256;
      for (unsigned x = 0; x < 256; ++x) dst[x] = 0xff000000u | src[x];
    }
    snes_ovl_fill_rect(panel->pixels, kRasterPanelWidth, kRasterPanelHeight,
                       13, 69 + (int)panel->row, 3, 1, SNES_OVL_COL_Y);
    snprintf(caption, sizeof(caption), "MAIN %02X   SUB %02X   VIEW %02X",
             (unsigned)view.registers.screenEnabled[0], (unsigned)view.registers.screenEnabled[1],
             panel->mask);
    RasterPanelText(panel, 18, 313, caption, 1);
    snprintf(caption, sizeof(caption), "CGADSUB %02X  CGWSEL %02X",
             (unsigned)view.registers.cgadsub, (unsigned)view.registers.cgwsel);
    RasterPanelText(panel, 18, 331, caption, 1);
    snprintf(caption, sizeof(caption), "FIXED COLOR %04X", (unsigned)view.registers.fixedColor);
    RasterPanelText(panel, 18, 349, caption, 1);
    snprintf(caption, sizeof(caption), "BG1 %u/%u   BG2 %u/%u",
             (unsigned)view.registers.hScroll[0], (unsigned)view.registers.vScroll[0],
             (unsigned)view.registers.hScroll[1], (unsigned)view.registers.vScroll[1]);
    RasterPanelText(panel, 18, 367, caption, 1);
    snprintf(caption, sizeof(caption), "BG3 %u/%u   BG4 %u/%u",
             (unsigned)view.registers.hScroll[2], (unsigned)view.registers.vScroll[2],
             (unsigned)view.registers.hScroll[3], (unsigned)view.registers.vScroll[3]);
    RasterPanelText(panel, 18, 382, caption, 1);

    RasterPanelText(panel, 302, 69, "CGRAM PALETTE", 1);
    for (unsigned color = 0; color < 256; ++color) {
      snes_ovl_fill_rect(panel->pixels, kRasterPanelWidth, kRasterPanelHeight,
                         302 + (int)(color % 16) * 11, 86 + (int)(color / 16) * 11,
                         10, 10, RasterPanelColor(view.palette[color]));
    }
    const SnesRasterInspectionObject *sprite = &view.sprite;
    snprintf(caption, sizeof(caption), "OBJ %u  X %d  Y %u", panel->sprite, sprite->x, sprite->y);
    RasterPanelText(panel, 302, 276, caption, 1);
    snprintf(caption, sizeof(caption), "TILE %03X  PRIORITY %u", sprite->tile, sprite->priority);
    RasterPanelText(panel, 302, 294, caption, 1);
    snprintf(caption, sizeof(caption), "SIZE %u  OBSEL %02X", sprite->size_select, sprite->obj_selection);
    RasterPanelText(panel, 302, 312, caption, 1);
    snprintf(caption, sizeof(caption), "PALETTE %02X-%02X", sprite->palette_base, sprite->palette_base + 15);
    RasterPanelText(panel, 302, 330, caption, 1);
    for (unsigned color = 0; color < 16; ++color)
      snes_ovl_fill_rect(panel->pixels, kRasterPanelWidth, kRasterPanelHeight,
                         302 + (int)color * 11, 347, 10, 10, RasterPanelColor(sprite->palette[color]));
  } else {
    RasterPanelText(panel, 18, 80, "CAPTURE UNAVAILABLE", 1);
  }
  RasterPanelText(panel, 18, 401, "LEFT/RIGHT FRAME   UP/DOWN LINE   PGUP/PGDN SPRITE", 1);
  RasterPanelText(panel, 18, 424, "1-4 BACKGROUNDS  5 OBJ  HOME/END RANGE  ESC CLOSE", 1);
  panel->dirty = false;
}

#endif
