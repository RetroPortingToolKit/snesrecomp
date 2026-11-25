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
 * in modes 1 and 7 (including native Mode 7), main-only clip/math, a frame
 * mixing 256- and 512-dot lines, a host that binds
 * nothing (renderBuffer unchanged, no marks), and an anchored widescreen
 * frame whose origin is not the centre.
 *
 * No game ROM, generated data, or platform frontend is required. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/snes.h"
#include "desktop/hires_present.h"

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

/* Mode 7 EXTBG resolves the same texel as blue BG1 on main and red BG2 on
 * sub: BG1 keeps palette index 0x82, BG2 uses its lower seven bits. */
static void setup_mode7_pseudo_hires(Ppu *ppu) {
    base_setup(ppu);
    ppu->bgmode = 7;
    ppu->setini = 0x48;                         /* EXTBG + pseudo hi-res */
    ppu->screenEnabled[0] = 1;
    ppu->screenEnabled[1] = 2;
    ppu->m7matrix[0] = ppu->m7matrix[3] = 0x100;
    for (size_t i = 0; i < sizeof ppu->vram / sizeof ppu->vram[0]; i++)
        ppu->vram[i] = 0x8201;                  /* tile 1, texel 0x82 */
    ppu->cgram[0x82] = kBlue;
    ppu->cgram[2] = kRed;
}

static bool pixel_is(const uint8_t *row, int column, uint16_t bgr15) {
    const uint8_t *p = row + (size_t)column * 4;   /* BGRX, full brightness */
    uint8_t r = (bgr15 & 0x1f) ? 0xff : 0, b = (bgr15 & 0x7c00) ? 0xff : 0;
    return p[0] == b && p[1] == 0 && p[2] == r && p[3] == 0;
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

        /* The surface contract exposes the raw resolved sub dot. Main-screen
         * clipping and addition must not accidentally modify that output. */
        setup_pseudo_hires(ppu);
        ppu->cgwsel = 0xc0;                     /* clip the main screen always */
        failures += check(PpuBindHiresSurface(ppu, sub_px, kPitch), "surface binds");
        draw(ppu, main_px, kPitch, kFlags[f], 1);
        failures += check(row_is(main_px, 0, 256, 0) && row_is(sub_px, 0, 256, kRed),
                          "main clip-to-black leaves the documented raw sub dot red");
        setup_pseudo_hires(ppu);
        ppu->cgwsel = 0x02;                     /* use the sub screen as operand */
        ppu->cgadsub = 0x01;                    /* add to main BG1 */
        failures += check(PpuBindHiresSurface(ppu, sub_px, kPitch), "surface binds");
        draw(ppu, main_px, kPitch, kFlags[f], 1);
        failures += check(row_is(main_px, 0, 256, kRed | kBlue) && row_is(sub_px, 0, 256, kRed),
                          "main addition produces magenta while the raw sub dot stays red");

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

        /* The selected native Mode 7 line service must publish the same raw
         * sub dot, including when main-only clip or math changes main output. */
        for (int effect = 0; effect < 4; effect++) {
            setup_mode7_pseudo_hires(ppu);
            if (effect == 1) ppu->cgwsel = 0xc0;
            if (effect == 2) { ppu->cgwsel = 0x02; ppu->cgadsub = 0x01; }
            if (effect == 3) ppu->inidisp |= 0x80;
            memset(sub_px, kSentinel, sizeof sub_px);
            failures += check(PpuBindHiresSurface(ppu, sub_px, kPitch), "Mode 7 surface binds");
            draw(ppu, main_px, kPitch, kFlags[f], 1);
            const uint16_t main_color = effect == 1 || effect == 3 ? 0 : effect == 2 ? kRed | kBlue : kBlue;
            const uint16_t sub_color = effect == 3 ? 0 : kRed;
            snprintf(msg, sizeof msg, "Mode 7 pseudo (%s, effect %d): main clip/math retained", renderer, effect);
            failures += check(row_is(main_px, 0, 256, main_color), msg);
            snprintf(msg, sizeof msg, "Mode 7 pseudo (%s, effect %d): raw sub dot and current row mark", renderer, effect);
            failures += check(row_is(sub_px, 0, 256, sub_color) && PpuHiresLine(ppu, 1) && PpuHiresFrame(ppu), msg);
        }

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
        {
            enum { kOutputWidth = 272, kOutputPitch = kOutputWidth * 8 + 16 };
            static uint32_t presented_words[kLines * kOutputPitch / 4];
            uint8_t *presented = (uint8_t *)presented_words;
            static uint32_t preview[kOutputWidth * kLines];
            SnesHiresField field = {main_px, sub_px, ppu->hiresLines,
                                    kPitch, kPitch, 256, kLines};
            memset(presented, kSentinel, sizeof presented_words);
            failures += check(SnesHiresPresent(presented, kOutputPitch, &field, kOutputWidth),
                              "presenter accepts a padded mixed-resolution field");
            for (int y = 0; y < kLines; ++y) {
                const uint8_t *row = presented + y * kOutputPitch;
                failures += check(row_is(row, 0, 16, 0) && row_is(row, 528, 16, 0),
                                  "presenter centers the authentic field in both margins");
                failures += check(row_untouched(row + kOutputWidth * 8, 16),
                                  "presenter respects destination pitch canaries");
                for (int x = 0; x < 256; ++x) {
                    failures += check(!memcmp(row + 16 * 4 + x * 8 + 4,
                                              main_px + y * kPitch + x * 4, 4),
                                      "odd physical dot preserves the original main screen");
                    if (y < 2)
                        failures += check(!memcmp(row + 16 * 4 + x * 8,
                                                  main_px + y * kPitch + x * 4, 4),
                                          "ordinary rows double each main column");
                    else
                        failures += check(pixel_is(row, 16 + x * 2, kRed) &&
                                          pixel_is(row, 17 + x * 2, kBlue),
                                          "hi-res rows preserve distinct red/blue physical dots");
                }
                SnesHiresPreview(preview + y * kOutputWidth,
                                 (const uint32_t *)row, kOutputWidth);
            }
            failures += check(preview[2 * kOutputWidth + 8] == 0x007f007f,
                              "logical-aspect preview averages both screens");
            failures += check(!SnesHiresPresent(presented, 256 * 8 - 1, &field, 256),
                              "short output pitches are rejected");
        }
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
        for (int scene = 0; scene < 4; scene++) {
            if (scene < 2) setup_true_hires(ppu, (uint8_t)(5 + scene));
            else if (scene == 2) setup_pseudo_hires(ppu);
            else setup_mode7_pseudo_hires(ppu);
            failures += check(PpuBindHiresSurface(ppu, sub_px, kPitch), "surface binds");
            draw(ppu, main_px, kPitch, kFlags[f], kLines);
            if (scene < 2) setup_true_hires(ppu, (uint8_t)(5 + scene));
            else if (scene == 2) setup_pseudo_hires(ppu);
            else setup_mode7_pseudo_hires(ppu);
            failures += check(PpuBindHiresSurface(ppu, NULL, 0), "surface unbinds");
            draw(ppu, plain_px, kPitch, kFlags[f], kLines);
            snprintf(msg, sizeof msg, "unbound (%s, scene %d): renderBuffer as with a surface", renderer, scene);
            failures += check(!memcmp(main_px, plain_px, sizeof main_px), msg);
            snprintf(msg, sizeof msg, "unbound (%s, scene %d): no hi-res marks", renderer, scene);
            failures += check(!PpuHiresFrame(ppu) && !PpuHiresLine(ppu, 1), msg);
        }
    }

    /* The native Mode 7 output shares the caller's anchored frame origin.
     * A one-row allocation with accessible canaries catches an extra offset
     * without asking the renderer to write outside the backing test object. */
    {
        enum { kExtra = 16, kGuard = 64, kWidePitch = (kPpuXPixels + 2 * kExtra) * 4 };
        static uint8_t wide_main[kGuard + kWidePitch + kGuard];
        static uint8_t wide_sub[kGuard + kWidePitch + kGuard];
        setup_mode7_pseudo_hires(ppu);
        PpuSetExtraSpace(ppu, kExtra);
        PpuSetWidescreenViewAnchor(ppu, true);
        PpuSetExtraSideSpace(ppu, 0, 2 * kExtra, 0);
        memset(wide_main, kSentinel, sizeof wide_main);
        memset(wide_sub, kSentinel, sizeof wide_sub);
        failures += check(PpuBindHiresSurface(ppu, wide_sub + kGuard, kWidePitch), "anchored Mode 7 surface binds");
        draw(ppu, wide_main + kGuard, kWidePitch, 0, 1);
        failures += check(row_is(wide_main + kGuard, 0, 256 + 2 * kExtra, kBlue) &&
                          row_is(wide_sub + kGuard, 0, 256 + 2 * kExtra, kRed) && PpuHiresLine(ppu, 1),
                          "anchored Mode 7: both surfaces use the frame origin and full row");
        failures += check(row_untouched(wide_main, kGuard) &&
                          row_untouched(wide_main + kGuard + kWidePitch, kGuard) &&
                          row_untouched(wide_sub, kGuard) &&
                          row_untouched(wide_sub + kGuard + kWidePitch, kGuard),
                          "anchored Mode 7: main/sub canaries stay untouched");
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

    /* The final overscan row is beam line 240. Its mark occupies slot zero
     * so the additive host metadata keeps the existing Ppu layout. Exercise
     * the captured-raster entry, which does not call the live line-1 reset. */
    {
        static uint32_t tall_main[240 * 256], tall_sub[240 * 256];
        static uint32_t tall_present[240 * 512];
        for (int mode7 = 0; mode7 < 2; ++mode7) {
            if (mode7) setup_mode7_pseudo_hires(ppu);
            else setup_true_hires(ppu, 5);
            memset(tall_main, 0, sizeof tall_main);
            memset(tall_sub, kSentinel, sizeof tall_sub);
            failures += check(PpuBindHiresSurface(ppu, (uint8_t *)tall_sub, kPitch),
                              "overscan sub surface binds");
            PpuBeginDrawing(ppu, (uint8_t *)tall_main, kPitch, kPpuRenderFlags_NewRenderer);
            ppu_renderRasterLine(ppu, 240);
            failures += check(PpuHiresFrame(ppu) && PpuHiresLine(ppu, 240) &&
                              !PpuHiresLine(ppu, 0) && !PpuHiresLine(ppu, 241),
                              "final visible overscan line is marked without changing layout");
            failures += check(row_is((uint8_t *)(tall_main + 239 * 256), 0, 256, kBlue) &&
                              row_is((uint8_t *)(tall_sub + 239 * 256), 0, 256, kRed),
                              "native Mode5/Mode7 final row exports both colors");
            SnesHiresField field = {(const uint8_t *)tall_main, (const uint8_t *)tall_sub,
                                    ppu->hiresLines, kPitch, kPitch, 256, 240};
            failures += check(SnesHiresPresent((uint8_t *)tall_present, 512 * 4, &field, 256),
                              "presenter accepts the full overscan field");
            for (int x = 0; x < 256; ++x) {
                const uint8_t *row = (const uint8_t *)(tall_present + 239 * 512);
                failures += check(pixel_is(row, x * 2, kRed) && pixel_is(row, x * 2 + 1, kBlue),
                                  "overscan last row preserves sub-even/main-odd phase");
            }
            PpuBeginDrawing(ppu, (uint8_t *)tall_main, kPitch, kPpuRenderFlags_NewRenderer);
            ppu->bgmode = 1; ppu->setini = 0;
            ppu_renderRasterLine(ppu, 1);
            failures += check(!PpuHiresFrame(ppu) && !PpuHiresLine(ppu, 240),
                              "beginning a captured field clears previous hi-res marks");
        }
    }

    ppu_free(ppu);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ppu_hires_surface_test: ok\n");
    return 0;
}
