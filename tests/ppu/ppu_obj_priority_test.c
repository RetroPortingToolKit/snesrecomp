/* Sprite priority against BG1/BG2 in the optimised renderer (NewRenderer).
 *
 * Modes 2-6 order the layers OBJ3, BG1.hi, OBJ2, BG2.hi, OBJ1, BG1.lo, OBJ0,
 * BG2.lo -- unlike mode 1, where BG1 and BG2 of each priority both sit above
 * the next sprite level. The optimised renderer drew modes 2 and 3 with mode
 * 1's priority bases, so a priority-1 sprite vanished behind a low-priority
 * BG1 tile (seen on a mode-3 title screen). Both renderers
 * draw an 8x8 sprite of each priority over an opaque BG layer of each
 * priority, the way the frame driver drives them (lines 1..8), and must agree.
 * Modes 4-6 take the compatibility renderer anyway. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;

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

enum { kLines = 8, kPitch = kPpuXPixels * 4 };

/* One BG layer (BG1 or BG2) of opaque colour 1 with tile priority `bg_prio`,
 * and an 8x8 sprite of colour 1 (palette 0) at the top left with `obj_prio`. */
static void setup(Ppu *ppu, uint8_t mode, int bg, int bg_prio, int obj_prio) {
    ppu_reset(ppu);
    ppu->inidisp = 0x0f;
    ppu->bgmode = mode;
    ppu->screenEnabled[0] = (uint8_t)(0x10 | (1 << bg));
    ppu->bgXsc[0] = 0x04;                /* BG1 tilemap at word 0x0400 */
    ppu->bgXsc[1] = 0x08;                /* BG2 tilemap at word 0x0800 */
    ppu->bgXsc[2] = 0x0c;                /* mode 2 offset-per-tile map: all zero */
    ppu->bgTileAdr = 0x0011;             /* BG1 and BG2 characters at word 0x1000 */
    memset(ppu->vram, 0, sizeof ppu->vram);
    const uint16_t entry = (uint16_t)(1 | (bg_prio ? 0x2000 : 0));
    for (int i = 0; i < 32 * 32; i++) ppu->vram[(bg ? 0x0800 : 0x0400) + i] = entry;
    const int words = (mode == 3 && bg == 0) ? 32 : 16;          /* BG1 is 8bpp in mode 3 */
    for (int row = 0; row < 8; row++) {
        ppu->vram[0x1000 + words + row] = 0x00ff;                /* plane 0: colour 1 */
        ppu->vram[row] = 0x00ff;                                 /* OBJ tile 0, colour 1 */
    }
    for (size_t i = 0; i < sizeof ppu->cgram / sizeof ppu->cgram[0]; i++)
        ppu->cgram[i] = 0;
    ppu->cgram[1] = 0x001f;                                      /* BG: red */
    ppu->cgram[128 + 1] = 0x03e0;                                /* OBJ palette 0: green */
    for (int i = 0; i < 128; i++) ppu->oam[i * 2] = 0xf000;      /* y = 240: off screen */
    memset(ppu->highOam, 0, sizeof ppu->highOam);
    ppu->oam[0] = 0x0000;                                        /* x 0, y 0 */
    ppu->oam[1] = (uint16_t)((obj_prio & 3) << 12);              /* tile 0, priority */
}

static void render_frame(Ppu *ppu, uint8_t *pixels, uint32_t flags) {
    memset(pixels, 0, (size_t)kPitch * (kLines + 1));
    PpuBeginDrawing(ppu, pixels, kPitch, flags);
    for (int line = 1; line <= kLines; line++)
        ppu_runLine(ppu, line);
}

static int compare(Ppu *ppu, uint8_t mode, int bg, int bg_prio, int obj_prio) {
    static uint8_t optimized[kPitch * (kLines + 1)], legacy[kPitch * (kLines + 1)];
    setup(ppu, mode, bg, bg_prio, obj_prio);
    render_frame(ppu, optimized, kPpuRenderFlags_NewRenderer);
    setup(ppu, mode, bg, bg_prio, obj_prio);
    render_frame(ppu, legacy, 0);
    for (int y = 0; y < kLines; y++) {
        for (int x = 0; x < 16; x++) {
            const uint8_t *a = optimized + (size_t)y * kPitch + (size_t)x * 4;
            const uint8_t *b = legacy + (size_t)y * kPitch + (size_t)x * 4;
            if (memcmp(a, b, 3) != 0) {
                fprintf(stderr, "FAIL: mode %u BG%d.%s OBJ%d: line %d x %d optimised %02x%02x%02x, "
                        "compatibility %02x%02x%02x\n", mode, bg + 1, bg_prio ? "hi" : "lo", obj_prio,
                        y + 1, x, a[2], a[1], a[0], b[2], b[1], b[0]);
                return 1;
            }
        }
    }
    return 0;
}

int main(void) {
    Ppu *ppu = ppu_init();
    int failures = 0;
    if (!ppu) return 2;
    static const uint8_t kModes[] = { 1, 2, 3 };
    for (size_t m = 0; m < sizeof kModes; m++)
        for (int bg = 0; bg < 2; bg++)
            for (int bg_prio = 0; bg_prio < 2; bg_prio++)
                for (int obj_prio = 0; obj_prio < 4; obj_prio++)
                    failures += compare(ppu, kModes[m], bg, bg_prio, obj_prio);
    if (failures) return 1;
    puts("ppu_obj_priority_test: PASS");
    return 0;
}
