/* Unit tests for runner/src/rom_patch.{c,h}: IPS and BPS applied in memory,
 * with every checksum enforced. Patches are constructed in the test so the
 * expected bytes are unambiguous. */
#include "rom_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc32.h"

static int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

typedef struct Buf { uint8_t data[4096]; size_t n; } Buf;
static void put(Buf *b, const void *p, size_t n) { memcpy(b->data + b->n, p, n); b->n += n; }
static void put8(Buf *b, unsigned v) { b->data[b->n++] = (uint8_t)v; }
static void put16be(Buf *b, unsigned v) { put8(b, v >> 8); put8(b, v & 0xff); }
static void put24be(Buf *b, unsigned v) { put8(b, v >> 16); put8(b, (v >> 8) & 0xff); put8(b, v & 0xff); }
static void put32le(Buf *b, uint32_t v) { for (int i = 0; i < 4; i++) put8(b, (v >> (8 * i)) & 0xff); }
static void putvar(Buf *b, uint64_t v) {
  for (;;) {
    uint8_t x = (uint8_t)(v & 0x7f);
    v >>= 7;
    if (v == 0) { put8(b, x | 0x80); break; }
    put8(b, x);
    v--;
  }
}

static void test_ips(void) {
  uint8_t src[64];
  for (int i = 0; i < 64; i++) src[i] = (uint8_t)i;
  Buf p = {{0}, 0};
  put(&p, "PATCH", 5);
  put24be(&p, 10); put16be(&p, 3); put(&p, "\xaa\xbb\xcc", 3);   /* plain */
  put24be(&p, 40); put16be(&p, 0); put16be(&p, 5); put8(&p, 0x77); /* RLE */
  put24be(&p, 64); put16be(&p, 2); put(&p, "\x01\x02", 2);         /* grows */
  put(&p, "EOF", 3);

  uint8_t *out = NULL; size_t n = 0;
  CHECK(snes_rom_patch_detect(p.data, p.n) == kRomPatchFormat_Ips);
  CHECK(snes_rom_patch_apply(src, 64, p.data, p.n, 1 << 20, &out, &n) == kRomPatch_Ok);
  CHECK(out && n == 66);
  if (out && n == 66) {
    CHECK(out[9] == 9 && out[10] == 0xaa && out[12] == 0xcc && out[13] == 13);
    for (int i = 40; i < 45; i++) CHECK(out[i] == 0x77);
    CHECK(out[45] == 45);
    CHECK(out[64] == 1 && out[65] == 2);
  }
  free(out);

  /* Growth past the cap is refused, not clamped. */
  CHECK(snes_rom_patch_apply(src, 64, p.data, p.n, 65, &out, &n) == kRomPatch_TooLarge);
  CHECK(out == NULL && n == 0);

  /* Truncation extension. */
  Buf t = {{0}, 0};
  put(&t, "PATCH", 5); put(&t, "EOF", 3); put24be(&t, 16);
  CHECK(snes_rom_patch_apply(src, 64, t.data, t.n, 1 << 20, &out, &n) == kRomPatch_Ok);
  CHECK(n == 16 && out && out[15] == 15);
  free(out);

  /* Malformed: record runs past the end; trailing junk. */
  Buf bad = {{0}, 0};
  put(&bad, "PATCH", 5); put24be(&bad, 0); put16be(&bad, 9); put(&bad, "xy", 2);
  CHECK(snes_rom_patch_apply(src, 64, bad.data, bad.n, 1 << 20, &out, &n) == kRomPatch_Invalid);
  Buf junk = {{0}, 0};
  put(&junk, "PATCH", 5); put(&junk, "EOF", 3); put8(&junk, 0);
  CHECK(snes_rom_patch_apply(src, 64, junk.data, junk.n, 1 << 20, &out, &n) == kRomPatch_Invalid);
  CHECK(snes_rom_patch_apply(src, 64, (const uint8_t *)"NOPE!!!!", 8, 1 << 20, &out, &n) == kRomPatch_Invalid);
}

/* Build a BPS that turns `src` into `tgt` using all four actions:
 *   SourceRead 8 | TargetRead 4 ("WXYZ") | SourceCopy 4 from offset 0 |
 *   TargetCopy 4 repeating the byte at out-1 (RLE idiom). */
static void build_bps(Buf *p, const uint8_t *src, size_t src_n,
                      const uint8_t *tgt, size_t tgt_n, int corrupt_target_crc) {
  p->n = 0;
  put(p, "BPS1", 4);
  putvar(p, src_n); putvar(p, tgt_n); putvar(p, 0);
  putvar(p, ((8 - 1) << 2) | 0);                 /* SourceRead 8 */
  putvar(p, ((4 - 1) << 2) | 1); put(p, "WXYZ", 4); /* TargetRead 4 */
  putvar(p, ((4 - 1) << 2) | 2); putvar(p, 0);   /* SourceCopy 4, delta +0 */
  putvar(p, ((4 - 1) << 2) | 3); putvar(p, (15 << 1) | 0); /* TargetCopy: rel = 15 */
  put32le(p, crc32_compute(src, src_n));
  put32le(p, crc32_compute(tgt, tgt_n) ^ (corrupt_target_crc ? 1u : 0u));
  put32le(p, crc32_compute(p->data, p->n));
}

static void test_bps(void) {
  uint8_t src[16];
  for (int i = 0; i < 16; i++) src[i] = (uint8_t)(0x10 + i);
  uint8_t tgt[20];
  memcpy(tgt, src, 8);
  memcpy(tgt + 8, "WXYZ", 4);
  memcpy(tgt + 12, src, 4);
  for (int i = 16; i < 20; i++) tgt[i] = tgt[15];   /* repeat byte 15 */

  Buf p = {{0}, 0};
  build_bps(&p, src, 16, tgt, 20, 0);
  uint8_t *out = NULL; size_t n = 0;
  CHECK(snes_rom_patch_detect(p.data, p.n) == kRomPatchFormat_Bps);
  CHECK(snes_rom_patch_apply(src, 16, p.data, p.n, 1 << 20, &out, &n) == kRomPatch_Ok);
  CHECK(n == 20 && out && memcmp(out, tgt, 20) == 0);
  free(out);

  /* Wrong source: size mismatch and CRC mismatch are both refused. */
  CHECK(snes_rom_patch_apply(src, 15, p.data, p.n, 1 << 20, &out, &n) == kRomPatch_SourceMismatch);
  uint8_t other[16]; memcpy(other, src, 16); other[3] ^= 0xff;
  CHECK(snes_rom_patch_apply(other, 16, p.data, p.n, 1 << 20, &out, &n) == kRomPatch_SourceMismatch);

  /* A patch whose own CRC is wrong is invalid; a bad target CRC is a
   * target mismatch (the bytes were produced, then refused). */
  p.data[6] ^= 1;
  CHECK(snes_rom_patch_apply(src, 16, p.data, p.n, 1 << 20, &out, &n) == kRomPatch_Invalid);
  build_bps(&p, src, 16, tgt, 20, 1);
  CHECK(snes_rom_patch_apply(src, 16, p.data, p.n, 1 << 20, &out, &n) == kRomPatch_TargetMismatch);
  CHECK(out == NULL);

  /* Result larger than the cap. */
  build_bps(&p, src, 16, tgt, 20, 0);
  CHECK(snes_rom_patch_apply(src, 16, p.data, p.n, 19, &out, &n) == kRomPatch_TooLarge);
}

int main(void) {
  test_ips();
  test_bps();
  CHECK(snes_rom_patch_status_text(kRomPatch_Ok)[0] != '\0');
  if (g_failures) {
    printf("%d failure(s)\n", g_failures);
    return 1;
  }
  printf("rom_patch_test: all checks passed\n");
  return 0;
}
