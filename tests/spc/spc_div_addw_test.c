/*
 * SPC700 DIV YA,X and ADDW YA,dp against the S-SMP's documented behaviour.
 *
 * The expected values follow bsnes (processor/spc700/instructions.cpp,
 * instructionDivide, and algorithms.cpp, algorithmADW):
 *   DIV:  H = (Y & 15) >= (X & 15), V = Y >= X. When Y < 2X the quotient fits in
 *         V + A and A = YA / X, Y = YA % X. Otherwise the S-SMP returns
 *         A = 255 - (YA - (X << 9)) / (256 - X) and Y = X + (YA - (X << 9)) % (256 - X).
 *         N and Z follow A alone. X = 0 takes the second form.
 *   ADDW: the 16-bit sum, with H the carry out of bit 11 and C the carry out of bit 15.
 *
 * Every X is checked against a stride through YA that always includes the edges, and every
 * ADDW pair of a coarse grid, so the run stays short while the quotient and half-carry
 * boundaries are all crossed.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"
#include "snes/snes.h"
#include "snes/spc.h"

/*
 * Host seams the APU pulls in. This harness builds apu/spc/dsp only, so the
 * rest of the machine and the audio trace rings are stubbed out.
 */
Snes *g_snes;
uint64_t g_apu_timer0_total_ticks;
int snes_frame_counter;

void cpu_saveload(Cpu *cpu, SaveLoadInfo *sli) { (void)cpu; (void)sli; }
void ppu_saveload(Ppu *ppu, SaveLoadInfo *sli) { (void)ppu; (void)sli; }
void dma_saveload(Dma *dma, SaveLoadInfo *sli) { (void)dma; (void)sli; }
void cart_saveload(Cart *cart, SaveLoadInfo *sli) { (void)cart; (void)sli; }

void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t value) {
  (void)port; (void)value;
}
void audio_trace_on_spc_port_read(uint8_t port, uint8_t value) {
  (void)port; (void)value;
}
void audio_trace_on_spc_port_write(uint8_t port, uint8_t value) {
  (void)port; (void)value;
}
void audio_trace_on_sample(int16_t left, int16_t right, int dropped,
                           uint32_t ring_fill) {
  (void)left; (void)right; (void)dropped; (void)ring_fill;
}
void audio_trace_on_reg_write(uint8_t address, uint8_t value) {
  (void)address; (void)value;
}
void audio_trace_on_consume(uint64_t read_index, uint32_t count,
                            uint32_t available_after) {
  (void)read_index; (void)count; (void)available_after;
}
void audio_trace_on_faithful_div(double divergence) { (void)divergence; }
void audio_trace_on_brr_compare(uint16_t block, uint8_t header, uint8_t sample,
                                int canon, int reference, int old, int older) {
  (void)block; (void)header; (void)sample; (void)canon;
  (void)reference; (void)old; (void)older;
}
void audio_trace_on_echo_div(double divergence) { (void)divergence; }
DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *shadow) { (void)shadow; }
void dsp_shadow_process(DspShadow *shadow, Dsp *dsp, int canonical_left,
                        int canonical_right, int *out_left, int *out_right) {
  (void)shadow; (void)dsp;
  *out_left = canonical_left;
  *out_right = canonical_right;
}

#define CODE_ADDR 0x0300u
#define DP_ADDR 0x10u

static unsigned g_failures;
static unsigned g_checked;

static void report(const char *what, unsigned x, unsigned ya, unsigned value)
{
    if (g_failures++ < 8)
        printf("FAIL %s X=%02x YA=%04x value=%04x\n", what, x, ya, value);
}

/* Runs the single instruction at CODE_ADDR with the registers set. */
static void runOne(Apu *apu, uint8_t a, uint8_t x, uint8_t y)
{
    Spc *spc = apu->spc;
    spc->a = a;
    spc->x = x;
    spc->y = y;
    spc->pc = CODE_ADDR;
    spc->stopped = false;
    spc_runOpcode(spc);
}

static void checkDiv(Apu *apu, unsigned x, unsigned ya)
{
    const uint8_t y = (uint8_t)(ya >> 8);
    uint8_t expectA;
    uint8_t expectY;

    if (y < (x << 1)) {
        expectA = (uint8_t)(ya / x);
        expectY = (uint8_t)(ya % x);
    } else {
        expectA = (uint8_t)(255 - (ya - (x << 9)) / (256 - x));
        expectY = (uint8_t)(x + (ya - (x << 9)) % (256 - x));
    }
    const bool expectV = y >= x;
    const bool expectH = (y & 15) >= (x & 15);

    apu->ram[CODE_ADDR] = 0x9e;
    runOne(apu, (uint8_t)ya, (uint8_t)x, y);
    Spc *spc = apu->spc;
    g_checked++;
    if (spc->a != expectA || spc->y != expectY || spc->v != expectV || spc->h != expectH ||
        spc->z != (expectA == 0) || spc->n != ((expectA & 0x80) != 0))
        report("DIV", x, ya, 0);
}

static void checkAddw(Apu *apu, unsigned ya, unsigned value)
{
    const unsigned sum = ya + value;
    const bool expectH = ((ya & 0xfff) + (value & 0xfff)) > 0xfff;
    const bool expectC = sum > 0xffff;
    const unsigned result = sum & 0xffff;

    apu->ram[CODE_ADDR] = 0x7a;
    apu->ram[CODE_ADDR + 1] = DP_ADDR;
    apu->ram[DP_ADDR] = (uint8_t)value;
    apu->ram[DP_ADDR + 1] = (uint8_t)(value >> 8);
    runOne(apu, (uint8_t)ya, 0, (uint8_t)(ya >> 8));
    Spc *spc = apu->spc;
    g_checked++;
    if ((unsigned)(spc->a | (spc->y << 8)) != result || spc->h != expectH || spc->c != expectC ||
        spc->z != (result == 0) || spc->n != ((result & 0x8000) != 0))
        report("ADDW", 0, ya, value);
}

int main(void)
{
    Apu *apu = apu_init();

    if (!apu) {
        fprintf(stderr, "apu_init failed\n");
        return 1;
    }
    apu_reset(apu);

    for (unsigned x = 0; x < 256; x++) {
        for (unsigned ya = 0; ya < 0x10000; ya += 37)
            checkDiv(apu, x, ya);
        for (unsigned ya = 0xff00; ya < 0x10000; ya++)
            checkDiv(apu, x, ya);
        for (unsigned ya = 0; ya < 0x200; ya++)
            checkDiv(apu, x, ya);
        /* the quotient boundary Y = 2X and its neighbours */
        for (unsigned low = 0; low < 256; low += 5) {
            const unsigned edge = (x << 1) & 0xff;
            checkDiv(apu, x, (edge << 8) | low);
            checkDiv(apu, x, (((edge - 1) & 0xff) << 8) | low);
            checkDiv(apu, x, (((edge + 1) & 0xff) << 8) | low);
        }
    }

    /* the half-carry sits at bit 11: a grid that crosses 0x0fff and 0x1000 in both operands */
    for (unsigned ya = 0; ya < 0x10000; ya += 0x111)
        for (unsigned value = 0; value < 0x10000; value += 0x77)
            checkAddw(apu, ya, value);
    for (unsigned ya = 0x0ff0; ya < 0x1010; ya++)
        for (unsigned value = 0x0ff0; value < 0x1010; value++)
            checkAddw(apu, ya, value);

    apu_free(apu);
    printf("%s: %u cases, %u failures\n", g_failures ? "FAIL" : "ok  ", g_checked, g_failures);
    return g_failures ? 1 : 0;
}
