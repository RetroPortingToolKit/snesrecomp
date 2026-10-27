/* Synthetic regression for PpuSetWidescreenViewAnchor
 * (WIDESCREEN_PATTERNS P2d: a bounded level slides the frame inside its own
 * bounds instead of pillarboxing at them).
 *
 * Without anchoring the frame origin is fixed -- native x=0 always lands at
 * framebuffer column extraLeftRight -- so a game that clamps a margin to its
 * level bound gets a black bar on that side and still only `budget` columns of
 * world on the other. With anchoring the two margins share one 2*budget
 * allowance and the origin follows them.
 *
 * The oracle is a REFERENCE render of the same scene at a symmetric 2*budget
 * margin, where every native column an anchored frame can reach is present at
 * a known place: what a column of the world looks like does not depend on how
 * much margin is live, only on where it is put. Every assertion here is
 * therefore of the form "frame column x + extraFrameLeft carries native
 * column x", which is the whole invariant.
 *
 * The load-bearing assertions:
 *   1. (L=0, R=2B) puts native x=0 at frame column 0 and world columns
 *      256..256+2B-1 across the right margin -- the level's left edge;
 *   2. (L=2B, R=0) is the mirror image, flush to the right physical edge;
 *   3. (0,0) and any (a,b) with a+b < 2B centre the live span, padding equally
 *      either side (an odd remainder goes right, since the origin floors);
 *   4. with anchoring OFF the origin is the fixed centre one for every
 *      (L<=B, R<=B), and the per-side clamp is unchanged -- the pre-change
 *      rule, which is the top acceptance criterion;
 *   5. the split BG3 HUD anchors its outer chunks to the PHYSICAL frame edges
 *      and keeps its centre group at the physical frame centre;
 *   6. budget 0 (authentic 256-wide output) is untouched either way;
 *   7. the new storage lies outside both PPU savestate spans.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;

/* ppu.c reaches runtime globals defined in snes.c / common_rtl.c and the
 * ws_shadow streaming-tilemap helpers. This harness links ppu.c on its own;
 * none of them participate in view anchoring. */
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;

uint16_t WsShadowTile(int layer, int screen_x, uint32_t wrapped_y,
                      uint16_t hscroll, uint16_t map_word_offset,
                      uint16_t real_tile) {
    (void)layer; (void)screen_x; (void)wrapped_y; (void)hscroll;
    (void)map_word_offset;
    return real_tile;
}
bool WsShadowLayerActive(int layer) { (void)layer; return false; }
uint32_t WsShadowWorldX(int layer) { (void)layer; return 0; }
uint32_t WsShadowPresentWorldY(int layer, int screen_x) {
    (void)layer; (void)screen_x; return 0;
}
uint32_t WsShadowScrollY(int layer) { (void)layer; return 0; }
void WsShadowOnVramWrite(uint16_t word_adr, uint16_t value) {
    (void)word_adr; (void)value;
}

enum {
    kBudget = 43,                      /* per-side centring budget */
    kSpan = kBudget * 2,               /* the anchored allowance, shared */
    kWide = kPpuXPixels + kSpan,       /* 342: the 16:9 frame */
    kRefExtra = kSpan,                 /* oracle render: 2B live BOTH sides */
    kRefWide = kPpuXPixels + kRefExtra * 2,
    kHudHeight = 16,
    kHudLeftEnd = 48,
    kHudRightStart = 208,
    kScroll = 128,                     /* mid-map: no tilemap wrap either way */
    kHudLine = 2,                      /* row 1: inside the HUD band */
    kWorldLine = 20,                   /* row 19: below it */
    kRows = 24,
    kMapBaseWords = 0x0000,            /* BG1 tilemap, 64x64 */
    kBg3MapBaseWords = 0x0800,         /* BG3 tilemap, 32x32 */
    kCharBaseWords = 0x1000,           /* BG1 4bpp characters */
    kBg3CharBaseWords = 0x2000,        /* BG3 2bpp characters */
    /* Spare columns after every row, so a deliberately inconsistent mutant
     * cannot corrupt the heap before the assertion that catches it runs. */
    kSlack = 64,
    kBg1 = 0x01,
    kBg3 = 0x04,
};

static int failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* Write one 4bpp tile row. Pixel 0 is leftmost (plane bit 7), matching the
 * hardware bit order the renderer decodes. */
static void put_tile_row(Ppu *ppu, int tile, int row, const int *px) {
    uint16_t w0 = 0, w1 = 0;
    for (int p = 0; p < 8; p++) {
        int v = px[p] & 0xf;
        int bit = 7 - p;
        w0 |= (uint16_t)(((v >> 0) & 1) << bit);
        w0 |= (uint16_t)(((v >> 1) & 1) << (bit + 8));
        w1 |= (uint16_t)(((v >> 2) & 1) << bit);
        w1 |= (uint16_t)(((v >> 3) & 1) << (bit + 8));
    }
    ppu->vram[(kCharBaseWords + tile * 16 + row) & 0x7fff] = w0;
    ppu->vram[(kCharBaseWords + tile * 16 + 8 + row) & 0x7fff] = w1;
}

/* Write one 2bpp tile row: plane 0 in the low byte, plane 1 in the high one. */
static void put_tile_row_2bpp(Ppu *ppu, int tile, int row, const int *px) {
    uint16_t w = 0;
    for (int p = 0; p < 8; p++) {
        int v = px[p] & 3;
        int bit = 7 - p;
        w |= (uint16_t)(((v >> 0) & 1) << bit);
        w |= (uint16_t)(((v >> 1) & 1) << (bit + 8));
    }
    ppu->vram[(kBg3CharBaseWords + tile * 8 + row) & 0x7fff] = w;
}

/* A Mode 1 scene whose every 8px column carries its own tile, so a rendered
 * pixel identifies the world column it came from and a displaced frame cannot
 * pass for a correctly placed one. BG1 is a 64x64 map (512px, wider than any
 * span reachable at kScroll); BG3 is a 32x32 HUD plane pinned to hScroll 0. */
static void setup_scene(Ppu *ppu, int layers) {
    ppu->inidisp = 0x0f;
    ppu->bgmode = 1;
    ppu->screenEnabled[0] = (uint8_t)layers;
    ppu->screenEnabled[1] = 0x00;
    ppu->screenWindowed[0] = 0x00;
    ppu->screenWindowed[1] = 0x00;
    ppu->bgXsc[0] = 0x03;           /* BG1 map at word 0, 64x64 */
    ppu->bgXsc[2] = 0x08;           /* BG3 map at word 0x800, 32x32 */
    ppu->bgTileAdr = 0x0201;        /* BG1 char 0x1000, BG3 char 0x2000 */
    ppu->vScroll[0] = 0;
    ppu->vScroll[2] = 0;
    ppu->hScroll[2] = 0;            /* the HUD plane is pinned, as HUDs are */

    memset(ppu->vram, 0, sizeof ppu->vram);
    ppu->cgram[0] = 0x0000;
    for (int i = 1; i < 256; i++)
        ppu->cgram[i] = (uint16_t)(0x0421 * (i & 0x1f) + 0x1004 * (i >> 5));

    /* 64 BG1 tile columns, tiles 1..64: a ramp shifted by the tile index. */
    for (int t = 1; t <= 64; t++)
        for (int row = 0; row < 8; row++) {
            int px[8];
            for (int p = 0; p < 8; p++)
                px[p] = ((t * 3 + p * 5 + row) % 15) + 1;
            put_tile_row(ppu, t, row, px);
        }
    for (int scr = 0; scr < 2; scr++)
        for (int row = 0; row < 32; row++)
            for (int col = 0; col < 32; col++)
                ppu->vram[kMapBaseWords + scr * 0x400 + row * 32 + col] =
                    (uint16_t)(scr * 32 + col + 1);

    /* 32 BG3 tile columns, tiles 1..32, each with its own palette/colour mix
     * so the HUD's three chunks are distinguishable from one another. */
    for (int t = 1; t <= 32; t++)
        for (int row = 0; row < 8; row++) {
            int px[8];
            for (int p = 0; p < 8; p++)
                px[p] = ((t + p + row) % 3) + 1;
            put_tile_row_2bpp(ppu, t, row, px);
        }
    for (int row = 0; row < 32; row++)
        for (int col = 0; col < 32; col++)
            ppu->vram[kBg3MapBaseWords + row * 32 + col] =
                (uint16_t)((col + 1) | ((col & 7) << 10));
}

/* One render of one scanline. `anchor` < 0 means "publish no per-side margins
 * at all" (the plain symmetric border); otherwise left/right are published with
 * anchoring in the requested state. Rows are `width + kSlack` columns apart;
 * the renderer must never touch the slack. Returns the rendered row. */
static uint32_t *render(Ppu *ppu, uint32_t *buf, int width, int budget,
                        int anchor, int left, int right, int layers,
                        bool hud_split, int line) {
    const size_t stride = (size_t)(width + kSlack);
    memset(buf, 0, sizeof(uint32_t) * stride * kRows);
    ppu_reset(ppu);
    PpuBeginDrawing(ppu, (uint8_t *)buf, sizeof(uint32_t) * stride,
                    kPpuRenderFlags_NewRenderer);
    setup_scene(ppu, layers);
    PpuSetExtraSpace(ppu, (uint16_t)budget);  /* resets policies: comes first */
    if (anchor >= 0) {
        PpuSetWidescreenViewAnchor(ppu, anchor != 0);
        PpuSetExtraSideSpace(ppu, left, right, 0);
    }
    if (hud_split) {
        PpuSetWidescreenHudSplit(ppu, kHudHeight, kHudLeftEnd, kHudRightStart);
        /* Always-visible is the configuration in which a split HUD is meant to
         * occupy the physical framebuffer edges even where the game has
         * clamped the world; without it the composite blacks the chunks that
         * fall outside the live world span, which is the existing
         * room-boundary pillarbox and not what this test is about. */
        PpuSetWidescreenHudAlwaysVisible(ppu, true);
    }
    ppu->hScroll[0] = kScroll;
    ppu_runLine(ppu, 0);
    ppu->hScroll[0] = kScroll;
    ppu_runLine(ppu, line);
    return buf + stride * (size_t)(line - 1);
}

/* Nothing may be written past the frame's last column. */
static void check_slack(const uint32_t *row, int width, const char *label) {
    for (int i = 0; i < kSlack; i++)
        if (row[width + i] != 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "%s: nothing written past the frame",
                     label);
            check(false, msg);
            return;
        }
}

/* Assert that a frame anchored with (left,right) places every live world
 * column where the invariant says it does, and that whatever the live span
 * does not claim is the equal black remainder. */
static void check_case(Ppu *ppu, uint32_t *buf, const uint32_t *ref, int left,
                       int right, int expect_frame_left, const char *label) {
    const uint32_t *out =
        render(ppu, buf, kWide, kBudget, 1, left, right, kBg1, false,
               kWorldLine);
    char msg[160];

    snprintf(msg, sizeof msg, "%s: live margins survive as asked", label);
    check(ppu->extraLeftCur == left && ppu->extraRightCur == right, msg);
    snprintf(msg, sizeof msg, "%s: frame origin is left + pad/2", label);
    check(ppu->extraFrameLeft == expect_frame_left, msg);
    snprintf(msg, sizeof msg,
             "%s: extraFrameLeft + extraFrameRight == 2*budget", label);
    check(ppu->extraFrameLeft + ppu->extraFrameRight == kSpan, msg);

    const int fl = (int)ppu->extraFrameLeft;
    int world_mismatch = 0, pad_mismatch = 0;
    for (int col = 0; col < kWide; col++) {
        int x = col - fl;                    /* native column at this place */
        if (x >= -left && x < kPpuXPixels + right) {
            if (out[col] != ref[x + kRefExtra])
                world_mismatch++;
        } else if (out[col] != 0) {
            pad_mismatch++;
        }
    }
    snprintf(msg, sizeof msg,
             "%s: frame column x + extraFrameLeft carries native column x",
             label);
    check(world_mismatch == 0, msg);
    snprintf(msg, sizeof msg, "%s: everything outside the live span is void",
             label);
    check(pad_mismatch == 0, msg);
    snprintf(msg, sizeof msg, "%s: padding is equal either side (+/-1)",
             label);
    check((fl - left) - (kSpan - fl - right) >= -1 &&
          (fl - left) - (kSpan - fl - right) <= 0, msg);
    check_slack(out, kWide, label);
}

int main(void) {
    Ppu *ppu = ppu_init();
    if (!ppu) return 2;

    const size_t ref_stride = (size_t)(kRefWide + kSlack);
    const size_t stride = (size_t)(kWide + kSlack);
    uint32_t *refbuf = calloc(ref_stride * kRows, sizeof(uint32_t));
    uint32_t *refbuf_hud = calloc(ref_stride * kRows, sizeof(uint32_t));
    uint32_t *buf = calloc(stride * kRows, sizeof(uint32_t));
    if (!refbuf || !refbuf_hud || !buf) {
        fprintf(stderr, "FAIL: oom\n");
        return 2;
    }
    /* Oracles: the same scene with 2*budget live on BOTH sides, so native
     * column x sits at frame column x + kRefExtra and every column an anchored
     * frame can reach is present. The HUD oracle is BG3 alone and unsplit --
     * the authentic source each chunk must reproduce. */
    const uint32_t *ref = render(ppu, refbuf, kRefWide, kRefExtra, -1, 0, 0,
                                 kBg1, false, kWorldLine);
    const uint32_t *ref_hud = render(ppu, refbuf_hud, kRefWide, kRefExtra, -1,
                                     0, 0, kBg3, false, kHudLine);

    /* 1/2. The two walls. At the left edge the game clamps its left margin to
     * zero and the whole 2*budget allowance becomes world on the right; at the
     * right edge the mirror image. */
    check_case(ppu, buf, ref, 0, kSpan, 0, "left wall (0, 2B)");
    check_case(ppu, buf, ref, kSpan, 0, kSpan, "right wall (2B, 0)");

    /* Spelled out, because it is what the whole feature is for. */
    {
        const uint32_t *out = render(ppu, buf, kWide, kBudget, 1, 0, kSpan,
                                     kBg1, false, kWorldLine);
        check(out[0] == ref[0 + kRefExtra],
              "left wall: native x=0 is at frame column 0");
        int margin_ok = 1;
        for (int k = 0; k < kSpan; k++)
            if (out[kPpuXPixels + k] != ref[kPpuXPixels + k + kRefExtra])
                margin_ok = 0;
        check(margin_ok,
              "left wall: world columns 256..256+2B-1 fill the right margin");

        out = render(ppu, buf, kWide, kBudget, 1, kSpan, 0, kBg1, false,
                     kWorldLine);
        check(out[kWide - 1] == ref[kPpuXPixels - 1 + kRefExtra],
              "right wall: native x=255 is at the last frame column");
        int left_ok = 1;
        for (int k = 1; k <= kSpan; k++)
            if (out[kSpan - k] != ref[-k + kRefExtra])
                left_ok = 0;
        check(left_ok,
              "right wall: world columns -1..-2B fill the left margin");
    }

    /* 3. Narrower than the frame: the live span centres with equal padding.
     * (0,0) is the pillarbox a bounded screen asks for; (a,b) with a+b < 2B is
     * a level only a little narrower than 16:9. The odd remainder goes right,
     * because extraFrameLeft takes the floor of pad/2. */
    check_case(ppu, buf, ref, 0, 0, kBudget, "narrow level (0, 0)");
    check_case(ppu, buf, ref, 10, 20, 10 + (kSpan - 30) / 2,
               "narrow level (10, 20)");
    check_case(ppu, buf, ref, 10, 21, 10 + (kSpan - 31) / 2,
               "odd pad (10, 21)");
    {
        render(ppu, buf, kWide, kBudget, 1, 10, 21, kBg1, false, kWorldLine);
        int pad_l = (int)ppu->extraFrameLeft - 10;
        int pad_r = kSpan - (int)ppu->extraFrameLeft - 21;
        check(pad_l == 27 && pad_r == 28,
              "odd pad: the spare column goes to the right side");
    }

    /* 4. Anchoring OFF keeps the fixed centre origin for every margin inside
     * the budget -- the rule every shipping consumer renders with today. */
    {
        static const int cases[][2] = { {0,0}, {20,30}, {43,43}, {5,43} };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            int l = cases[i][0], r = cases[i][1];
            const uint32_t *out = render(ppu, buf, kWide, kBudget, 0, l, r,
                                         kBg1, false, kWorldLine);
            char msg[160];
            snprintf(msg, sizeof msg,
                     "anchoring off (%d,%d): origin is the fixed budget", l, r);
            check(ppu->extraFrameLeft == kBudget &&
                  ppu->extraFrameRight == kBudget, msg);
            int bad = 0;
            for (int col = 0; col < kWide; col++) {
                int x = col - kBudget;
                if (x >= -l && x < kPpuXPixels + r) {
                    if (out[col] != ref[x + kRefExtra]) bad++;
                } else if (out[col] != 0) {
                    bad++;
                }
            }
            snprintf(msg, sizeof msg,
                     "anchoring off (%d,%d): native x sits at frame column x+B",
                     l, r);
            check(bad == 0, msg);
            check_slack(out, kWide, "anchoring off");
        }
        /* The per-side clamp to the budget is unchanged too: asking for more
         * than the budget with anchoring off still yields the budget. */
        render(ppu, buf, kWide, kBudget, 0, kSpan, kSpan, kBg1, false,
               kWorldLine);
        check(ppu->extraLeftCur == kBudget && ppu->extraRightCur == kBudget,
              "anchoring off: per-side margins still clamp to the budget");
    }

    /* 5. The split BG3 HUD. Its outer chunks ride the PHYSICAL frame edges and
     * its centre group rides the SLID origin -- a fixed-origin render puts the
     * outer chunks in the same two places but leaves the centre group at
     * left_end + budget instead of left_end + extraFrameLeft. */
    {
        static const struct { int left, right, fl; const char *label; } hud[] = {
            { 0,     kSpan, 0,       "hud at the left wall" },
            { kSpan, 0,     kSpan,   "hud at the right wall" },
            { 0,     0,     kBudget, "hud on a narrow level" },
        };
        for (size_t i = 0; i < sizeof hud / sizeof *hud; i++) {
            const uint32_t *out = render(ppu, buf, kWide, kBudget, 1,
                                         hud[i].left, hud[i].right, kBg3, true,
                                         kHudLine);
            const int fl = hud[i].fl;
            char msg[160];
            int bad = 0;

            check(ppu->extraFrameLeft == fl,
                  "hud case: the origin is where the margins put it");
            /* Left chunk: source [0,left_end) flush to physical column 0. */
            for (int k = 0; k < kHudLeftEnd; k++)
                if (out[k] != ref_hud[k + kRefExtra]) bad++;
            snprintf(msg, sizeof msg,
                     "%s: left chunk is flush with the physical left edge",
                     hud[i].label);
            check(bad == 0, msg);
            /* Right chunk: source [right_start,256) flush to the last column. */
            bad = 0;
            for (int k = kHudRightStart; k < kPpuXPixels; k++)
                if (out[k + kSpan] != ref_hud[k + kRefExtra]) bad++;
            snprintf(msg, sizeof msg,
                     "%s: right chunk is flush with the physical right edge",
                     hud[i].label);
            check(bad == 0, msg);
            /* Centre group: stationary in screen space as the world slides. */
            bad = 0;
            for (int k = kHudLeftEnd; k < kHudRightStart; k++)
                if (out[k + kBudget] != ref_hud[k + kRefExtra]) bad++;
            snprintf(msg, sizeof msg,
                     "%s: centre group stays at the physical centre", hud[i].label);
            check(bad == 0, msg);
            check_slack(out, kWide, hud[i].label);
        }
        /* The HUD band must not have disturbed the rows below it. */
        const uint32_t *out = render(ppu, buf, kWide, kBudget, 1, 0, kSpan,
                                     kBg1, true, kWorldLine);
        int bad = 0;
        for (int col = 0; col < kWide; col++)
            if (out[col] != ref[col + kRefExtra]) bad++;
        check(bad == 0,
              "hud split: rows below the band still carry the anchored world");
    }

    /* 6. Budget 0: authentic 256-wide output, anchoring on or off. */
    {
        const size_t narrow = (size_t)(kPpuXPixels + kSlack);
        uint32_t *a = calloc(narrow * kRows, sizeof(uint32_t));
        uint32_t *b = calloc(narrow * kRows, sizeof(uint32_t));
        if (a && b) {
            const uint32_t *plain = render(ppu, a, kPpuXPixels, 0, -1, 0, 0,
                                           kBg1 | kBg3, false, kWorldLine);
            const uint32_t *anchored = render(ppu, b, kPpuXPixels, 0, 1, 0, 0,
                                              kBg1 | kBg3, false, kWorldLine);
            check(memcmp(plain, anchored,
                         sizeof(uint32_t) * kPpuXPixels) == 0,
                  "budget 0: anchoring leaves authentic 256-wide output alone");
            check(ppu->extraFrameLeft == 0 && ppu->extraFrameRight == 0,
                  "budget 0: the origin is 0 either side");
        } else {
            fprintf(stderr, "FAIL: oom\n");
            failures++;
        }
        free(a);
        free(b);
    }

    /* The invariant, swept over every pair a game could publish -- including
     * the out-of-range ones the setter has to absorb. */
    {
        int bad = 0;
        for (int l = 0; l <= kSpan + 8; l++)
            for (int r = 0; r <= kSpan + 8; r++) {
                PpuSetExtraSpace(ppu, kBudget);
                PpuSetWidescreenViewAnchor(ppu, true);
                PpuSetExtraSideSpace(ppu, l, r, 0);
                if (ppu->extraFrameLeft + ppu->extraFrameRight != kSpan ||
                    ppu->extraFrameLeft < ppu->extraLeftCur ||
                    ppu->extraFrameRight < ppu->extraRightCur ||
                    ppu->extraLeftCur + ppu->extraRightCur > kSpan)
                    bad++;
            }
        check(bad == 0,
              "sweep: the origin spans 2*budget and always covers both margins");
    }

    /* Enabling the anchor re-derives the origin from the margins already
     * published, so a game may publish the two in either order within a frame.
     * (The margins themselves stay whatever the last publish clamped them to,
     * which is why a game re-publishes both every frame.) */
    {
        PpuSetExtraSpace(ppu, kBudget);
        PpuSetExtraSideSpace(ppu, 0, kSpan, 0);
        check(ppu->extraLeftCur == 0 && ppu->extraRightCur == kBudget &&
              ppu->extraFrameLeft == kBudget,
              "anchor off: a level-edge margin clamps to the budget");
        PpuSetWidescreenViewAnchor(ppu, true);
        check(ppu->extraFrameLeft == (kSpan - kBudget) / 2 &&
              ppu->extraFrameRight == kSpan - (kSpan - kBudget) / 2,
              "enabling the anchor re-derives the origin in place");
        PpuSetWidescreenViewAnchor(ppu, false);
        check(ppu->extraFrameLeft == kBudget &&
              ppu->extraFrameRight == kBudget,
              "disabling the anchor restores the fixed centre origin");
        /* PpuSetExtraSpace* reset it, like every other widescreen policy. */
        PpuSetWidescreenViewAnchor(ppu, true);
        PpuSetExtraSpace(ppu, kBudget);
        check(ppu->wsViewAnchor == 0,
              "PpuSetExtraSpace resets the anchor with the other policies");
        ppu_reset(ppu);
        check(ppu->wsViewAnchor == 0 && ppu->extraFrameLeft == 0 &&
              ppu->extraFrameRight == 0,
              "ppu_reset zeroes the anchor state with the other ws fields");
    }

    /* 7. The new storage is host state: it must sit between the register
     * snapshot and the memory snapshot, in neither. */
    check(offsetof(Ppu, wsViewAnchor) > offsetof(Ppu, cgwsel) &&
          offsetof(Ppu, extraFrameLeft) > offsetof(Ppu, cgwsel) &&
          offsetof(Ppu, extraFrameRight) + sizeof(uint16_t) <=
              offsetof(Ppu, cgram),
          "view-anchor storage lies outside both PPU savestate spans");

    free(refbuf);
    free(refbuf_hud);
    free(buf);
    ppu_free(ppu);
    printf("ppu_view_anchor_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
