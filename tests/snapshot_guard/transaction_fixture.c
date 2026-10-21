#include <assert.h>
#include <stddef.h>
#include "common_rtl.h"
#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "snes/snes.h"
#include "snes/dsp.h"
#include "snes/spc.h"
#include "snes/joypad.h"

static Snes machine;
static Cpu cpu;
static Apu apu;
static Dsp dsp;
static Spc spc;
static Dma dma;
static Ppu ppu;
static Cart cart;
static unsigned char ram[0x20000];
static unsigned char sram[32];
Snes *g_snes = &machine;
CpuState g_cpu;
int snes_frame_counter;
static unsigned host_extra;
static unsigned reconciled;
static unsigned presentation_resets;
static unsigned locks;
static unsigned audio_resets;
static void rtl_reset_audio_delivery(void) { ++audio_resets; }
void snes_mod_audio_stop_all(void) {}
const RtlGameInfo *g_rtl_game_info;

void RtlApuLock(void) { ++locks; }
void RtlApuUnlock(void) { assert(locks); --locks; }
void PpuResetWidescreenOamHistory(Ppu *p) { (void)p; ++presentation_resets; }
void cart_saveload(Cart *c, SaveLoadInfo *s) { s->func(s, c->ram, c->ramSize); }
static void save_extra(SaveLoadInfo *s) { s->func(s, &host_extra, sizeof(host_extra)); }
static void load_extra(SaveLoadInfo *s, uint32_t version) {
    (void)version;
    s->func(s, &host_extra, sizeof(host_extra));

}
static bool validate_extra(const void *data, size_t size, uint32_t version) {
    (void)version;
    return (size==1 || size==sizeof(host_extra)) && *(const uint8_t *)data==17;
}
static void load_variable(SaveLoadInfo *s, uint32_t version) {
    (void)version;
    if (RtlStateBytesRemaining(s)==1) {
        uint8_t small=0; s->func(s,&small,1); host_extra=small;
    } else s->func(s,&host_extra,sizeof(host_extra));
}
static const char *guard_identity(void) { return "transaction-test"; }
static bool title_preflight(const void *data, size_t size) {
    (void)data; (void)size; assert(!locks); return false;
}
static void reconcile(uint32_t version) { (void)version; ++reconciled; }
static const RtlGameInfo game = {
    .state_save_extra = save_extra, .state_load_extra = load_extra,
    .on_state_loaded = reconcile,
};
static uint32_t s_state_generation;
