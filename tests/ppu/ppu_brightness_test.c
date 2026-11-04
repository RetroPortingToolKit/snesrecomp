/* Regression: INIDISP brightness is applied to the 5-bit colour with
 * rounding and then expanded to 8 bits, as bsnes does
 * (lightTable: round(c * l / 15), then (c << 3) | (c >> 2)). The renderers
 * expanded first and scaled the 8-bit value without rounding, which left
 * fades a few levels off.
 *
 * The backdrop (CGRAM 0) is drawn with no layer enabled, through both the
 * optimized and the compatibility renderer. The raster jobs' colour cache is
 * checked against the same rounding for every component and level. No game
 * ROM required. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/raster_register_jobs.h"

Snes *g_snes;

/* ppu.c references these runtime globals, but this harness links ppu.c alone. */
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;

uint16_t WsShadowTile(int layer, int screen_x, uint32_t wrapped_y,
                      uint16_t real_tile) {
    (void)layer;
    (void)screen_x;
    (void)wrapped_y;
    return real_tile;
}

bool WsShadowLayerActive(int layer) {
    (void)layer;
    return false;
}

uint32_t WsShadowWorldX(int layer) {
    (void)layer;
    return 0;
}

uint32_t WsShadowPresentWorldY(int layer, int screen_x) {
    (void)layer;
    (void)screen_x;
    return 0;
}

uint32_t WsShadowScrollY(int layer) {
    (void)layer;
    return 0;
}

void WsShadowOnVramWrite(uint16_t word_adr, uint16_t value) {
    (void)word_adr;
    (void)value;
}

static int check(bool condition, const char *message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition ? 0 : 1;
}

/* Red channel of pixel 0 for backdrop colour (red component c) at brightness l. */
static int red_at(Ppu *ppu, unsigned c, unsigned l, uint32_t flags) {
    uint8_t pixels[kPpuXPixels * 4];
    ppu_reset(ppu);
    ppu->inidisp = (uint8_t)l;
    ppu->bgmode = 1;
    ppu->cgram[0] = (uint16_t)c;
    memset(pixels, 0, sizeof pixels);
    PpuBeginDrawing(ppu, pixels, sizeof pixels, flags);
    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
    return pixels[2];   /* BGRX */
}

int main(void) {
    Ppu *ppu = ppu_init();
    int failures = 0;
    if (!ppu) return 2;
    static const struct { unsigned c, l; int want; } kCases[] = {
        { 31, 15, 255 },   /* full brightness unchanged */
        { 31,  7, 115 },   /* round(31*7/15)=14 -> 115 (was 119) */
        {  8,  1,   8 },   /* round(8/15)=1 -> 8 (was 4) */
        { 16,  8,  74 },   /* round(16*8/15)=9 -> 74 */
        { 31,  0,   0 },
    };
    static const uint32_t kFlags[] = { 0, kPpuRenderFlags_NewRenderer };
    for (size_t f = 0; f < 2; f++)
        for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
            char msg[96];
            int got = red_at(ppu, kCases[i].c, kCases[i].l, kFlags[f]);
            snprintf(msg, sizeof msg, "%s renderer: colour %u at brightness %u -> %d (got %d)",
                     f ? "optimized" : "compatibility", kCases[i].c, kCases[i].l, kCases[i].want, got);
            failures += check(got == kCases[i].want, msg);
        }
    /* Raster jobs: every 5-bit component at every level. */
    static SnesRenderColorCache cache;
    for (unsigned l = 0; l < 16; l++) {
        const uint32_t *rgb = SnesRenderBrightness(&cache, l);
        for (unsigned c = 0; c < 32; c++) {
            unsigned v = (2u * c * l + 15u) / 30u;          /* round(c * l / 15) */
            unsigned want = (v << 3) | (v >> 2);
            unsigned got = (rgb[c] >> 16) & 0xff;            /* red: low 5 bits */
            if (got != want) {
                fprintf(stderr, "FAIL: raster jobs: colour %u at brightness %u -> %u (got %u)\n",
                        c, l, want, got);
                failures++;
            }
        }
    }
    ppu_free(ppu);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ppu_brightness_test: ok\n");
    return 0;
}
