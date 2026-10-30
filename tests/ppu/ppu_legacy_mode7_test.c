/* Compare both build-selected image services from identical live inputs.
 * The reference translation unit is compiled separately with its public
 * entry renamed; production never executes two renderers. */
#define main composition_fixture_main
#include "ppu_composition_regression_test.c"
#undef main
#include "snes/ppu_legacy.h"

void ppu_draw_whole_line_reference(Ppu *, int);

static uint32_t reference[kPpuBufWidth * 240], candidate[kPpuBufWidth * 240];
static uint16_t captured_vram[0x8000];

int main(void) {
  Ppu *a = ppu_init(), *b = ppu_init();
  uint32_t random = 0xf178a59b;
  uint64_t digest = 1469598103934665603ull;
  unsigned counts[2][2] = {{0}}, lines = 0;
  for (unsigned sample = 0; sample < 16384; ++sample) {
    if (!(sample & 63)) {
      fill_render_case(a, &random);
      for (unsigned i = 0; i < 0x8000; ++i) captured_vram[i] = (uint16_t)rng_next(&random);
    }
    a->bgmode = 7;
    a->inidisp = (uint8_t)(rng_next(&random) & 143);
    a->setini = (uint8_t)rng_next(&random);
    a->m7sel = (uint8_t)rng_next(&random);
    a->mosaic = (uint8_t)rng_next(&random);
    a->mosaicStartLine = rng_next(&random) % 225;
    for (unsigned i = 0; i < 8; ++i) a->m7matrix[i] = (int16_t)rng_next(&random);
    /* Identity and zero transforms, map overflow and transparent texels
     * complement arbitrary signed/fractional matrices. */
    if (!(sample & 7)) {
      a->m7matrix[0] = a->m7matrix[3] = sample & 8 ? 256 : 0;
      a->m7matrix[1] = a->m7matrix[2] = 0;
    }
    a->screenEnabled[0] = (uint8_t)rng_next(&random);
    a->screenEnabled[1] = (uint8_t)rng_next(&random);
    a->screenWindowed[0] = (uint8_t)rng_next(&random);
    a->screenWindowed[1] = (uint8_t)rng_next(&random);
    a->windowsel = rng_next(&random);
    a->wbgobjlog = (uint16_t)rng_next(&random);
    a->window1left = (uint8_t)rng_next(&random);
    a->window1right = (uint8_t)rng_next(&random);
    a->window2left = (uint8_t)rng_next(&random);
    a->window2right = (uint8_t)rng_next(&random);
    a->cgwsel = (uint8_t)rng_next(&random);
    a->cgadsub = (uint8_t)rng_next(&random);
    a->fixedColor = (uint16_t)rng_next(&random);
    PpuSetExtraSpace(a, rng_next(&random) % (kPpuExtraLeftRight + 1));
    PpuSetExtraSideSpace(a, rng_next(&random) % (a->extraLeftRight + 1),
                            rng_next(&random) % (a->extraLeftRight + 1), 0);
    a->extraObjectCount = sample & 1;
    a->renderVram = sample & 2 ? captured_vram : NULL;
    g_snes_ppu_dbg_layer_mask = (uint8_t)rng_next(&random);
    for (unsigned i = 0; i < kPpuBufWidth; ++i) {
      /* Include real ranks, transparent and arbitrary malformed ranks. */
      a->objBuffer.data[i] = sample & 4 ? (uint16_t)rng_next(&random) :
          (uint16_t)((SPRITE_PRIO_TO_PRIO(rng_next(&random) & 3, false) << 8) |
          (rng_next(&random) & 255));
      a->objectColors[i] = (uint16_t)rng_next(&random);
    }
    unsigned line = 1 + rng_next(&random) % 239;
    memset(reference, 0xa5, sizeof(reference));
    memcpy(candidate, reference, sizeof(candidate));
    a->renderBuffer = (uint8_t *)reference;
    a->renderPitch = kPpuBufWidth * sizeof(uint32_t);
    *b = *a;
    b->renderBuffer = (uint8_t *)candidate;
    ppu_draw_whole_line_reference(a, line);
    ppu_draw_whole_line_legacy(b, line);
    if (memcmp(reference, candidate, sizeof(reference))) {
      for (unsigned x = 0; x < kPpuBufWidth * 240; ++x)
        if (reference[x] != candidate[x]) {
          fprintf(stderr, "Mode7 mismatch sample=%u line=%u x=%d ref=%08x native=%08x m7=%02x mosaic=%02x ext=%u direct=%u\n",
              sample, line, (int)(x % kPpuBufWidth) - a->extraLeftRight,
              reference[x], candidate[x], a->m7sel, a->mosaic,
              !!PPU_m7extBg(a), !!PPU_directColor(a));
          return 1;
        }
    }
    b->renderBuffer = a->renderBuffer;
    if (memcmp(a, b, sizeof(*a))) {
      fprintf(stderr, "Image job changed PPU state sample=%u\n", sample);
      return 1;
    }
    ++counts[!!PPU_m7extBg(a)][!!PPU_directColor(a)];
    ++lines;
    digest = hash_bytes(digest, reference + (line - 1) * kPpuBufWidth,
                         kPpuBufWidth * sizeof(uint32_t));
  }
  printf("legacy_mode7: PASS lines=%u digest=%016llx ext/direct=%u,%u,%u,%u\n",
      lines, (unsigned long long)digest, counts[0][0], counts[0][1], counts[1][0], counts[1][1]);
  free(a); free(b);
  return 0;
}
