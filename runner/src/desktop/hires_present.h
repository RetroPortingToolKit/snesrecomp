#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One completed PPU field. Width and pitch count the original main-screen
 * columns; line marks use the PPU's beam numbering, modulo 240 (row zero
 * is line one, and the last overscan row uses mark zero).
 * The two surfaces and marks remain owned by the producer. */
typedef struct SnesHiresField {
  const uint8_t *main_pixels, *sub_pixels, *lines;
  size_t main_pitch, sub_pitch;
  int width, height;
} SnesHiresField;

/* Compose physical dots without changing the field's logical geometry.
 * Mixed-resolution fields double each ordinary column and interleave the
 * sub/main dots only on marked rows. A wider destination centers the native
 * field; game-owned composition uses its additive draw_hires_frame hook. */
static inline bool SnesHiresPresent(uint8_t *dst, size_t pitch,
                                   const SnesHiresField *field, int width) {
  if (!dst || !field || !field->main_pixels || !field->sub_pixels ||
      !field->lines || field->width <= 0 || field->height <= 0 ||
      field->height > 240 || width < field->width || width > INT32_MAX / 8 ||
      ((width - field->width) & 1) || pitch < (size_t)width * 8 ||
      field->main_pitch < (size_t)field->width * 4 ||
      field->sub_pitch < (size_t)field->width * 4)
    return false;
  const int left = (width - field->width) / 2;
  for (int y = 0; y < field->height; ++y) {
    uint8_t *row = dst + (size_t)y * pitch;
    const uint8_t *main = field->main_pixels + (size_t)y * field->main_pitch;
    const uint8_t *sub = field->lines[(y + 1) % 240]
        ? field->sub_pixels + (size_t)y * field->sub_pitch : main;
    memset(row, 0, (size_t)width * 8);
    row += (size_t)left * 8;
    for (int x = 0; x < field->width; ++x) {
      memcpy(row + (size_t)x * 8, sub + (size_t)x * 4, 4);
      memcpy(row + (size_t)x * 8 + 4, main + (size_t)x * 4, 4);
    }
  }
  return true;
}

/* Thumbnails keep the field's logical aspect. Average each pair of physical
 * dots so both screens contribute instead of discarding every even column. */
static inline void SnesHiresPreview(uint32_t *dst, const uint32_t *src,
                                   size_t pixels) {
  for (size_t x = 0; x < pixels; ++x) {
    uint32_t a = src[x * 2], b = src[x * 2 + 1];
    dst[x] = ((a & 0x00fefefeu) >> 1) + ((b & 0x00fefefeu) >> 1) +
             (a & b & 0x00010101u);
  }
}
