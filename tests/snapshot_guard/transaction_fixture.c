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
static unsigned save_captures;
static unsigned extra_loads;
static void rtl_reset_audio_delivery(void) { ++audio_resets; }
void snes_mod_audio_stop_all(void) {}
const RtlGameInfo *g_rtl_game_info;

void RtlApuLock(void) { ++locks; }
void RtlApuUnlock(void) { assert(locks); --locks; }
void PpuResetWidescreenOamHistory(Ppu *p) { (void)p; ++presentation_resets; }
void cart_saveload(Cart *c, SaveLoadInfo *s) { s->func(s, c->ram, c->ramSize); }
static void save_extra(SaveLoadInfo *s) {
    ++save_captures; /* Legacy saves may capture title state, as MMX does. */
    s->func(s, &host_extra, sizeof(host_extra));
}
static void load_extra(SaveLoadInfo *s, uint32_t version) {
    (void)version;
    ++extra_loads;
    s->func(s, &host_extra, sizeof(host_extra));

}
static bool validate_title_extra(const void *data, size_t size) {
    assert(!locks); /* Existing preflight may query the guest layout. */
    size_t core=RtlSnapshotGuestSize(NULL);
    if (size<core) return false;
    if (size==core) return true;
    size_t extra=size-core;
    const uint8_t *chunk=(const uint8_t *)data+core;
    if (extra==1) return chunk[0]==17;
    if (extra!=sizeof(host_extra)) return false;
    unsigned value; memcpy(&value,chunk,sizeof(value)); return value==17;
}
static void load_variable(SaveLoadInfo *s, uint32_t version) {
    (void)version;
    ++extra_loads;
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
    .snapshot_preflight = validate_title_extra,
};
static uint32_t s_state_generation;
