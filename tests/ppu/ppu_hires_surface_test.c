/* Hi-res sub-screen surface (PpuBindHiresSurface).
 *
 * In modes 5/6 and with pseudo hi-res ($2133 bit 3) every 256-pixel column
 * holds two dots: the sub screen in the even one, the main screen in the odd
 * one. renderBuffer keeps the main screen; a bound surface receives the sub
 * screen of the lines drawn in hi-res, and PpuHiresLine/PpuHiresFrame say
 * which lines those were.
 *
 * Each case gives the two columns different colours (red sub, blue main), so
 * a swapped or missing column shows. Covered: modes 5 and 6, pseudo hi-res
 * in mode 1, a frame mixing 256- and 512-dot lines, a host that binds
 * nothing (renderBuffer unchanged, no marks), and an anchored widescreen
 * frame whose origin is not the centre.
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

enum {
    kPitch = kPpuXPixels * 4,
    kLines = 4,
    kRed = 0x001f,
    kBlue = 0x7c00,
    kSentinel = 0xa5,
};

static int check(bool condition, const char *message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition ? 0 : 1;
}

/* A 4bpp tile whose even pixels are colour `even` and odd pixels colour
 * `odd` (each 1 or 2). */
static void fill_tile(Ppu *ppu, uint16_t tile_base, int even, int odd) {
    uint8_t plane0 = 0, plane1 = 0;
    for (int px = 0; px < 8; px++) {
        int color = (px & 1) ? odd : even;
        uint8_t bit = (uint8_t)(0x80 >> px);
        if (color & 1) plane0 |= bit;
        if (color & 2) plane1 |= bit;
    }
    for (int row = 0; row < 8; row++) {
        ppu->vram[tile_base + row] = (uint16_t)(plane0 | plane1 << 8);
        ppu->vram[tile_base + 8 + row] = 0;
    }
}

static void base_setup(Ppu *ppu) {
    ppu_reset(ppu);
    ppu->inidisp = 0x0f;
    ppu->bgXsc[0] = 0x00;
    ppu->bgXsc[1] = 0x00;
    memset(ppu->vram, 0, sizeof ppu->vram);
    for (int i = 0; i < 32 * 32; i++) ppu->vram[i] = 1;   /* every entry: tile 1 */
    for (size_t i = 0; i < sizeof ppu->cgram / sizeof ppu->cgram[0]; i++)
        ppu->cgram[i] = 0;
    ppu->cgram[1] = kRed;
    ppu->cgram[2] = kBlue;
}

/* Mode 5 or 6: BG1 on main and sub screen; its 512-dot pattern alternates red
 * (even dots, sub column) and blue (odd dots, main column). */
static void setup_true_hires(Ppu *ppu, uint8_t mode) {
    base_setup(ppu);
    ppu->bgmode = mode;
    ppu->screenEnabled[0] = 1;
    ppu->screenEnabled[1] = 1;
    ppu->bgTileAdr = 0x0001;                     /* BG1 characters at 0x1000 */
    fill_tile(ppu, 0x1000 + 1 * 16, 1, 2);
    fill_tile(ppu, 0x1000 + 2 * 16, 1, 2);       /* right half of the 16-dot tile */
}

/* Mode 1: BG1 (solid blue) on the main screen, BG2 (solid red) on the sub
 * screen; pseudo hi-res shows the sub screen in the even column. */
static void setup_pseudo_hires(Ppu *ppu) {
    base_setup(ppu);
    ppu->bgmode = 1;
    ppu->setini = 0x08;
    ppu->screenEnabled[0] = 1;
    ppu->screenEnabled[1] = 2;
    ppu->bgTileAdr = 0x0021;                     /* BG1 at 0x1000, BG2 at 0x2000 */
    fill_tile(ppu, 0x1000 + 1 * 16, 2, 2);
    fill_tile(ppu, 0x2000 + 1 * 16, 1, 1);
}

static bool pixel_is(const uint8_t *row, int column, uint16_t bgr15) {
    const uint8_t *p = row + (size_t)column * 4;   /* BGRX, full brightness */
    uint8_t r = (bgr15 & 0x1f) ? 0xff : 0, b = (bgr15 & 0x7c00) ? 0xff : 0;
    return p[0] == b && p[1] == 0 && p[2] == r;
}

/* Lines 0..count of one frame. */
static void draw(Ppu *ppu, uint8_t *pixels, size_t pitch, uint32_t flags, int count) {
    PpuBeginDrawing(ppu, pixels, pitch, flags);
    for (int line = 0; line <= count; line++) ppu_runLine(ppu, line);
}

static bool row_is(const uint8_t *row, int first, int count, uint16_t color) {
    for (int x = first; x < first + count; x++)
        if (!pixel_is(row, x, color)) return false;
    return true;
}

static bool row_untouched(const uint8_t *row, size_t bytes) {
    for (size_t i = 0; i < bytes; i++)
        if (row[i] != kSentinel) return false;
    return true;
}

int main(void) {
    static uint8_t main_px[kLines * kPitch], sub_px[kLines * kPitch];
    static uint8_t plain_px[kLines * kPitch];
    static const uint32_t kFlags[] = { 0, kPpuRenderFlags_NewRenderer };
    Ppu *ppu = ppu_init();
    int failures = 0;
    char msg[128];
    if (!ppu) return 2;

    for (size_t f = 0; f < 2; f++) {
        const char *renderer = kFlags[f] ? "new renderer" : "compatibility renderer";

        /* Modes 5 and 6: blue main column, red sub column, every line marked. */
        for (uint8_t mode = 5; mode <= 6; mode++) {
            setup_true_hires(ppu, mode);
            memset(sub_px, kSentinel, sizeof sub_px);
            failures += check(PpuBindHiresSurface(ppu, sub_px, kPitch), "surface binds");
            draw(ppu, main_px, kPitch, kFlags[f], 2);
            snprintf(msg, sizeof msg, "mode %u (%s): renderBuffer is the main screen (blue)", mode, renderer);
            failures += check(row_is(main_px, 0, 256, kBlue) && row_is(main_px + kPitch, 0, 256, kBlue), msg);
            snprintf(msg, sizeof msg, "mode %u (%s): surface is the sub screen (red)", mode, renderer);
            failures += check(row_is(sub_px, 0, 256, kRed) && row_is(sub_px + kPitch, 0, 256, kRed), msg);
            snprintf(msg, sizeof msg, "mode %u (%s): lines 1 and 2 are marked hi-res", mode, renderer);
            failures += check(PpuHiresLine(ppu, 1) && PpuHiresLine(ppu, 2) && PpuHiresFrame(ppu), msg);
        }

        /* Pseudo hi-res in mode 1. */
        setup_pseudo_hires(ppu);
        memset(sub_px, kSentinel, sizeof sub_px);
        failures += check(PpuBindHiresSurface(ppu, sub_px, kPitch), "surface binds");
        draw(ppu, main_px, kPitch, kFlags[f], 1);
        snprintf(msg, sizeof msg, "pseudo hi-res (%s): main BG1 blue, sub BG2 red", renderer);
        failures += check(row_is(main_px, 0, 256, kBlue) && row_is(sub_px, 0, 256, kRed) &&
                          PpuHiresLine(ppu, 1), msg);

        /* Nothing on the sub screen: the even dot is the backdrop, CGRAM[0],
         * not the fixed colour (COLDATA), as hardware does in hi-res. */
        setup_pseudo_hires(ppu);
        ppu->screenEnabled[1] = 0;
        ppu->cgram[0] = kRed;
        ppu->fixedColor = kBlue;
        memset(sub_px, kSentinel, sizeof sub_px);
        failures += check(PpuBindHiresSurface(ppu, sub_px, kPitch), "surface binds");
        draw(ppu, main_px, kPitch, kFlags[f], 1);
        snprintf(msg, sizeof msg, "pseudo hi-res (%s): empty sub screen shows CGRAM[0], not COLDATA", renderer);
        failures += check(row_is(sub_px, 0, 256, kRed), msg);

        /* One frame, lines 1-2 in mode 1 (256 dots), lines 3-4 in mode 5. */
        setup_true_hires(ppu, 1);
        memset(sub_px, kSentinel, sizeof sub_px);
        failures += check(PpuBindHiresSurface(ppu, sub_px, kPitch), "surface binds");
        PpuBeginDrawing(ppu, main_px, kPitch, kFlags[f]);
        for (int line = 0; line <= 2; line++) ppu_runLine(ppu, line);
        ppu->bgmode = 5;
        for (int line = 3; line <= kLines; line++) ppu_runLine(ppu, line);
        snprintf(msg, sizeof msg, "mixed frame (%s): only lines 3-4 are marked", renderer);
        failures += check(!PpuHiresLine(ppu, 1) && !PpuHiresLine(ppu, 2) &&
                          PpuHiresLine(ppu, 3) && PpuHiresLine(ppu, 4) && PpuHiresFrame(ppu), msg);
        snprintf(msg, sizeof msg, "mixed frame (%s): surface rows of 256-dot lines are not written", renderer);
        failures += check(row_untouched(sub_px, kPitch) && row_untouched(sub_px + kPitch, kPitch), msg);
        snprintf(msg, sizeof msg, "mixed frame (%s): 512-dot lines split into blue main and red sub", renderer);
        failures += check(row_is(main_px + 2 * kPitch, 0, 256, kBlue) && row_is(sub_px + 2 * kPitch, 0, 256, kRed) &&
                          row_is(main_px + 3 * kPitch, 0, 256, kBlue) && row_is(sub_px + 3 * kPitch, 0, 256, kRed), msg);
        /* The next frame clears the marks once its line 1 is drawn, also for
         * a frame-model host that never passes line 0. */
        ppu->bgmode = 1;
        draw(ppu, main_px, kPitch, kFlags[f], 1);
        snprintf(msg, sizeof msg, "mixed frame (%s): marks are cleared by the next frame", renderer);
        failures += check(!PpuHiresLine(ppu, 3) && !PpuHiresFrame(ppu), msg);
        ppu->bgmode = 5;
        draw(ppu, main_px, kPitch, kFlags[f], 1);
        ppu->bgmode = 1;
        PpuBeginDrawing(ppu, main_px, kPitch, kFlags[f]);
        ppu_runLine(ppu, 1);
        snprintf(msg, sizeof msg, "frame model (%s): line 1 without line 0 clears the marks", renderer);
        failures += check(!PpuHiresLine(ppu, 1) && !PpuHiresFrame(ppu), msg);

        /* A host that binds nothing gets the same renderBuffer and no marks. */
        for (int scene = 0; scene < 3; scene++) {
            if (scene < 2) setup_true_hires(ppu, (uint8_t)(5 + scene)); else setup_pseudo_hires(ppu);
            failures += check(PpuBindHiresSurface(ppu, sub_px, kPitch), "surface binds");
            draw(ppu, main_px, kPitch, kFlags[f], kLines);
            if (scene < 2) setup_true_hires(ppu, (uint8_t)(5 + scene)); else setup_pseudo_hires(ppu);
            failures += check(PpuBindHiresSurface(ppu, NULL, 0), "surface unbinds");
            draw(ppu, plain_px, kPitch, kFlags[f], kLines);
            snprintf(msg, sizeof msg, "unbound (%s, scene %d): renderBuffer as with a surface", renderer, scene);
            failures += check(!memcmp(main_px, plain_px, sizeof main_px), msg);
            snprintf(msg, sizeof msg, "unbound (%s, scene %d): no hi-res marks", renderer, scene);
            failures += check(!PpuHiresFrame(ppu) && !PpuHiresLine(ppu, 1), msg);
        }
    }

    /* Anchored widescreen: the origin slides to frame column 0, away from the
     * centre (extraLeftRight); the sub column must follow renderBuffer. */
    {
        enum { kExtra = 16, kWidePitch = (kPpuXPixels + 2 * kExtra) * 4 };
        static uint8_t wide_main[kWidePitch * 2], wide_sub[kWidePitch * 2];
        setup_true_hires(ppu, 5);
        PpuSetExtraSpace(ppu, kExtra);
        PpuSetWidescreenViewAnchor(ppu, true);
        PpuSetExtraSideSpace(ppu, 0, 2 * kExtra, 0);
        failures += check(ppu->extraFrameLeft == 0 && ppu->extraLeftRight == kExtra,
                          "anchored: native x=0 sits at frame column 0");
        memset(wide_sub, kSentinel, sizeof wide_sub);
        failures += check(PpuBindHiresSurface(ppu, wide_sub, kWidePitch), "wide surface binds");
        draw(ppu, wide_main, kWidePitch, 0, 1);
        failures += check(row_is(wide_main, 0, 256, kBlue) && row_is(wide_sub, 0, 256, kRed),
                          "anchored: main and sub columns share the frame origin");
    }

    ppu_free(ppu);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ppu_hires_surface_test: ok\n");
    return 0;
}
