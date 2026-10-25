/* Regression: BG modes the optimized renderer does not implement (4, 5, 6)
 * must come out of the same scanline as the compatibility renderer instead
 * of falling through to the mode-7 path.
 *
 * Seen on a mode 5 menu (hi-res text over a 4bpp background): with
 * NewRenderer=1 the screen was vertical colour stripes, because the mode
 * dispatch in PpuDrawWholeLine knew modes 0-3 and treated everything else
 * as mode 7, so BG1's tilemap was sampled as a mode-7 pixel map.
 *
 * No game ROM, generated data, or platform frontend is required. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/snes.h"

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

/* One solid 4bpp tile (16 words) at word address tile_base. */
static void fill_solid_4bpp_tile(Ppu *ppu, uint16_t tile_base, uint8_t color) {
    uint16_t lo = 0, hi = 0;
    for (int bit = 0; bit < 8; bit++) {
        if (color & 1) lo |= (uint16_t)(1u << bit);
        if (color & 2) lo |= (uint16_t)(1u << (8 + bit));
        if (color & 4) hi |= (uint16_t)(1u << bit);
        if (color & 8) hi |= (uint16_t)(1u << (8 + bit));
    }
    for (int row = 0; row < 8; row++) {
        ppu->vram[tile_base + row] = lo;
        ppu->vram[tile_base + 8 + row] = hi;
    }
}

/* BG1 only, tilemap at word 0, characters at word 0x1000. Every tilemap
 * entry names tile 1; in the 16-pixel-wide tile modes the right half is
 * tile 2. Tile 1 is colour 1, tile 2 is colour 2, so a hi-res scanline is
 * 8 pixels of colour 1 followed by 8 of colour 2, repeating. */
static void setup_two_tone_bg1(Ppu *ppu, uint8_t mode) {
    ppu_reset(ppu);
    ppu->inidisp = 0x0f;
    ppu->bgmode = mode;
    ppu->screenEnabled[0] = 1;
    ppu->bgXsc[0] = 0x00;
    ppu->bgTileAdr = 0x0001;
    memset(ppu->vram, 0, sizeof ppu->vram);
    for (int i = 0; i < 32 * 32; i++) ppu->vram[i] = 1;
    fill_solid_4bpp_tile(ppu, 0x1000 + 1 * 16, 1);
    fill_solid_4bpp_tile(ppu, 0x1000 + 2 * 16, 2);
    for (size_t i = 0; i < sizeof ppu->cgram / sizeof ppu->cgram[0]; i++)
        ppu->cgram[i] = 0;
    ppu->cgram[1] = 0x001f;  /* red */
    ppu->cgram[2] = 0x7c00;  /* blue */
}

static void render_line(Ppu *ppu, uint8_t *pixels, size_t pitch, uint32_t flags) {
    memset(pixels, 0, pitch);
    PpuBeginDrawing(ppu, pixels, pitch, flags);
    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
}

static bool pixel_is(const uint8_t *pixels, int x, uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t *p = pixels + (size_t)x * 4;  /* BGRX */
    return p[0] == b && p[1] == g && p[2] == r;
}

int main(void) {
    enum { kPitch = kPpuXPixels * 4 };
    uint8_t legacy[kPitch], optimized[kPitch];
    Ppu *ppu = ppu_init();
    int failures = 0;
    if (!ppu) return 2;

    static const uint8_t kModes[] = { 4, 5, 6 };
    for (size_t m = 0; m < sizeof kModes; m++) {
        char msg[96];
        uint8_t mode = kModes[m];

        setup_two_tone_bg1(ppu, mode);
        render_line(ppu, legacy, kPitch, 0);
        setup_two_tone_bg1(ppu, mode);
        render_line(ppu, optimized, kPitch, kPpuRenderFlags_NewRenderer);

        snprintf(msg, sizeof msg, "mode %u: optimized renderer matches the compatibility renderer", mode);
        failures += check(!memcmp(legacy, optimized, kPitch), msg);

        snprintf(msg, sizeof msg, "mode %u: compatibility renderer draws the layer (not backdrop)", mode);
        failures += check(!pixel_is(legacy, 0, 0, 0, 0), msg);
    }

    /* Mode 5 samples the main screen from the odd hi-res column, so the
     * 256-wide output shows 4 pixels of tile 1 then 4 of tile 2. */
    setup_two_tone_bg1(ppu, 5);
    render_line(ppu, optimized, kPitch, kPpuRenderFlags_NewRenderer);
    failures += check(pixel_is(optimized, 0, 0xff, 0, 0) && pixel_is(optimized, 3, 0xff, 0, 0),
                      "mode 5: pixels 0-3 come from the left 8 hi-res columns (tile 1, red)");
    failures += check(pixel_is(optimized, 4, 0, 0, 0xff) && pixel_is(optimized, 7, 0, 0, 0xff),
                      "mode 5: pixels 4-7 come from the right 8 hi-res columns (tile 2, blue)");
    failures += check(pixel_is(optimized, 8, 0xff, 0, 0),
                      "mode 5: the 16-pixel tile repeats at pixel 8");

    /* Modes 0-3 and 7 stay on the optimized path: a mode-1 scene must not
     * change because of the fallback. */
    setup_two_tone_bg1(ppu, 1);
    render_line(ppu, legacy, kPitch, 0);
    setup_two_tone_bg1(ppu, 1);
    render_line(ppu, optimized, kPitch, kPpuRenderFlags_NewRenderer);
    failures += check(pixel_is(optimized, 0, 0xff, 0, 0) && pixel_is(optimized, 7, 0xff, 0, 0) &&
                      pixel_is(optimized, 8, 0xff, 0, 0),
                      "mode 1: 8-pixel tiles, tile 1 fills pixels 0-15");

    ppu_free(ppu);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ppu_hires_mode_test: ok\n");
    return 0;
}
