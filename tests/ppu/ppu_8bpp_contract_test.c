/* Compile this unchanged against the scalar and batched PPU. The shared
 * fixture supplies link stubs; each executable contains one renderer only. */
#define main composition_test_main
#include "ppu_composition_regression_test.c"
#undef main

static int known_planes(Ppu *p) {
  ppu_reset(p);
  p->bgmode = 3;
  p->screenEnabled[0] = 1;
  p->bgXsc[0] = 4; /* map starts at word 0x400 */
  p->vram[0] = 0x4080;
  p->vram[8] = 0x1020;
  p->vram[16] = 0x0408;
  p->vram[24] = 0x0102;
  for (int flip = 0; flip < 2; flip++) {
    p->vram[0x400] = flip ? 0x4000 : 0;
    memset(&p->bgBuffers, 0, sizeof(p->bgBuffers));
    PpuDrawBackground_8bpp(p, 0, false, 0, 0xc000, 0x8000);
    for (int x = 0; x < 8; x++) {
      unsigned want = 0x8000 + (1u << (flip ? 7 - x : x));
      if (p->bgBuffers[0].data[kPpuExtraLeftRight + x] != want)
        return fail_at("known_planes", flip, x,
            p->bgBuffers[0].data[kPpuExtraLeftRight + x], want);
    }
  }
  return 0;
}

static void random_case(Ppu *p, uint32_t *rng, unsigned n) {
  ppu_reset(p);
  PpuSetExtraSpaceCentered(p, (n & 1) ? 48 : 0);
  p->inidisp = 15;
  p->bgmode = 3 | ((n & 2) ? 0x10 : 0);
  p->screenEnabled[0] = p->screenEnabled[1] = 1;
  p->screenWindowed[0] = p->screenWindowed[1] = (n & 4) ? 1 : 0;
  p->windowsel = rng_next(rng) & 0xffffff;
  p->window1left = (uint8_t)rng_next(rng);
  p->window1right = (uint8_t)rng_next(rng);
  p->window2left = (uint8_t)rng_next(rng);
  p->window2right = (uint8_t)rng_next(rng);
  p->bgXsc[0] = (uint8_t)rng_next(rng);
  p->bgTileAdr = (uint16_t)rng_next(rng);
  p->hScroll[0] = (uint16_t)rng_next(rng);
  p->vScroll[0] = (uint16_t)rng_next(rng);
  for (unsigned i = 0; i < 0x8000; i++)
    p->vram[i] = (uint16_t)rng_next(rng);
  for (unsigned s = 0; s < 2; s++)
    for (unsigned i = 0; i < sizeof(p->bgBuffers[s].data) / sizeof(PpuZbufType); i++)
      p->bgBuffers[s].data[i] = (PpuZbufType)rng_next(rng);
}

int main(int argc, char **argv) {
  Ppu *p = ppu_init();
  if (!p || known_planes(p)) return 1;
  uint32_t rng = 0x59b3f971;
  uint64_t hash = 1469598103934665603ull;
  if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
    int loops = argc > 2 ? atoi(argv[2]) : 2000;
    if (loops < 1 || loops > 100000) return 2;
    random_case(p, &rng, 2);
    p->screenWindowed[0] = 0;
    p->bgXsc[0] = 7;
    p->hScroll[0] = 3;
    clock_t start = clock();
    for (int n = 0; n < loops; n++)
      for (unsigned y = 0; y < 224; y++) {
        memset(&p->bgBuffers[0], 0, sizeof(p->bgBuffers[0]));
        PpuDrawBackground_8bpp(p, y, false, 0, 0xc000, 0x8000);
      }
    double ms = 1000.0 * (clock() - start) / CLOCKS_PER_SEC;
    hash = hash_bytes(hash, &p->bgBuffers, sizeof(p->bgBuffers));
    printf("{\"loops\":%d,\"ms\":%.3f,\"hash\":\"%016llx\"}\n",
           loops, ms, (unsigned long long)hash);
  } else {
    for (unsigned n = 0; n < 4096; n++) {
      random_case(p, &rng, n);
      PpuDrawBackground_8bpp(p, rng_next(&rng) % 240, n & 1, 0, 0xc000, 0x8000);
      hash = hash_bytes(hash, &p->bgBuffers, sizeof(p->bgBuffers));
      hash = hash_bytes(hash, p->vram, sizeof(p->vram));
    }
    printf("ppu_8bpp_contract cases=4096 hash=%016llx\n", (unsigned long long)hash);
  }
  ppu_free(p);
  return 0;
}
