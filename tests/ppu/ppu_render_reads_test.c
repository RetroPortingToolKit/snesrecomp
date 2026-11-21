/* Real register reads, with the register-access beam supplied by the host.
 * Reuse the existing PPU fixture's unrelated host stubs. */
#define main ppu_sprite_limit_harness_main
#include "ppu_sprite_limit_test.c"
#undef main
#include <assert.h>
#include <stdlib.h>

extern bool (*g_ppu_beam_hook)(uint16_t, uint16_t *, uint16_t *);
static uint16_t clock_h = 128, clock_v = 1;
static bool beam_ready = true;
static bool beam(uint16_t adr, uint16_t *h, uint16_t *v) {
    assert(adr == 0x2138 || adr == 0x213b);
    *h = clock_h; *v = clock_v; return beam_ready;
}
static void cgram_address(Ppu *p) { p->cgramPointer = 9; p->cgramSecondWrite = false; }
static void check_cgram_pair(Ppu *p, unsigned word) {
    cgram_address(p);
    assert(ppu_read(p, 0x3b) == (word & 0xff));
    assert(p->cgramPointer == 9 && p->cgramSecondWrite);
    assert(ppu_read(p, 0x3b) == ((word >> 8) & 0x7f));
    assert(p->cgramPointer == 10 && !p->cgramSecondWrite);
}
int main(int argc, char **argv) {
    const bool on = argc > 1 && strcmp(argv[1], "on") == 0;
    Ppu *p = ppu_init(); assert(p); ppu_reset(p);
    ppu_write(p, 0, 0x0f); /* active display */
    p->cgram[0] = 0x1234; p->cgram[9] = 0x5678;
    g_ppu_beam_hook = beam;
    p->screenEnabled[1] = 0;
    check_cgram_pair(p, on ? 0x1234 : 0x5678);
    /* Explicitly retained scope: sub-screen palette fetches are not modeled. */
    p->screenEnabled[1] = 1;
    check_cgram_pair(p, 0x5678);
    p->screenEnabled[1] = 0;
    clock_h = 0; check_cgram_pair(p, 0x5678);
    clock_h = 1096; check_cgram_pair(p, 0x5678);
    clock_h = 128; clock_v = 225; check_cgram_pair(p, 0x5678);
    clock_v = 0; check_cgram_pair(p, 0x5678);
    clock_v = 1; beam_ready = false; check_cgram_pair(p, 0x5678);
    beam_ready = true; ppu_write(p, 0, 0x8f); check_cgram_pair(p, 0x5678);
    ppu_write(p, 0, 0x0f);
    /* H=4: range evaluation fetches sprite1's X byte; CPU latch still walks. */
    clock_h = 4; p->oam[2] = 0x3412; p->oam[5] = 0x7856;
    p->oamAdr = 5; p->oamInHigh = false; p->oamSecondWrite = false;
    assert(ppu_read(p, 0x38) == (on ? 0x12 : 0x56));
    assert(p->oamAdr == 5 && p->oamSecondWrite);
    assert(ppu_read(p, 0x38) == (on ? 0x12 : 0x78));
    assert(p->oamAdr == 6 && !p->oamSecondWrite);
    ppu_write(p, 0, 0x8f); p->oamAdr = 5;
    assert(ppu_read(p, 0x38) == 0x56);
    assert(ppu_read(p, 0x38) == 0x78 && p->oamAdr == 6);
    ppu_free(p);
    puts("render reads: active/blank/free/unsupported-subscreen and CPU latch progression PASS");
    return 0;
}
