"""ROM-free tests of the real host pacing code with a deterministic device."""
import os
from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"(?m)^(?:static )?(?:void|bool) " + name + r"\([^;]*?\)\s*\{", source)
    assert match, f"missing producer API: {name}"
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def run_c(tmp_path, source):
    compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")
    if not compiler:
        pytest.skip("C compiler required")
    cfile = tmp_path / "producer.c"
    cfile.write_text(source, encoding="utf-8")
    exe = tmp_path / ("producer.exe" if os.name == "nt" else "producer")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)


STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint32_t sampleRead, sampleWrite; } Dsp;
static Dsp dsp;
static struct Apu { Dsp *dsp; uint64_t portClock; } apu = { &dsp, 0 };
static struct Machine { struct Apu *apu; } machine = {&apu};
static struct Machine *g_snes = &machine;
static void *g_audio_mutex;
static _Thread_local unsigned g_apu_lock_depth;
static _Thread_local bool g_audio_producer_active;
static _Thread_local bool g_audio_consumer_stalled;
static _Thread_local uint32_t g_audio_stalled_read;
static uint64_t g_audio_callback_count;
static bool g_audio_fast_forward, g_rtl_speculative_frame;
static void (*g_audio_producer_wait)(void);
static _Thread_local bool g_audio_producer_wait_active;
static uint32_t ticks, delays;
static bool consume, callback_only;
static unsigned locked;
#define HOST_AUDIO_HIGH_WATER 4096u
static void SDL_LockMutex(void *mutex) { (void)mutex; ++locked; }
static void SDL_UnlockMutex(void *mutex) { (void)mutex; assert(locked); --locked; }
static uint32_t dsp_available(Dsp *d) { return d->sampleWrite - d->sampleRead; }
static uint32_t SDL_GetTicks(void) { return ticks; }
static void SDL_Delay(unsigned ms) {
  assert(!locked && "pacing must leave the consumer mutex unlocked");
  ticks += ms; ++delays;
  if (consume) dsp.sampleRead += 32;
  if (callback_only) ++g_audio_callback_count;
}
'''


def test_callback_activity_does_not_rearm_stalled_device(tmp_path):
    host = (ROOT / "runner/src/desktop/host_main.c").read_text(encoding="utf-8")
    rtl = (ROOT / "runner/src/common_rtl.c").read_text(encoding="utf-8")
    source = STUBS
    source += function(rtl, "RtlAudioSetProducerWait")
    source += function(rtl, "RtlAudioWaitForConsumer")
    source += "\nstatic void HostWaitForAudioProducer(void);\n"
    source += function(host, "RtlApuLock") + function(host, "RtlApuUnlock")
    source += function(host, "HostWaitForAudioProducer")
    source += r'''
int main(void) {
  RtlAudioSetProducerWait(HostWaitForAudioProducer);
  dsp.sampleWrite = 7000;
  RtlApuLock(); RtlApuUnlock(); /* No active consumer (paused/headless). */
  assert(!delays && !locked);
  g_audio_producer_active = true;
  consume = true;
  RtlApuLock(); RtlApuUnlock();
  assert(delays == 91 && dsp.sampleRead == 2912 && !locked);
  assert(dsp.sampleWrite == 7000 && "pacing must not discard PCM");
  consume = false; delays = 0; dsp.sampleRead = 0;
  ticks = UINT32_MAX - 100; /* Stall timeout must survive SDL tick wrap. */
  dsp.sampleWrite = 7000; g_audio_producer_active = true;
  RtlApuLock(); RtlApuUnlock();
  assert(delays == 250 && !locked);
  for (unsigned word = 0; word < 100; ++word) {
    ++g_audio_callback_count; /* Callbacks can run without consuming PCM. */
    g_audio_producer_active = true;
    RtlApuLock(); RtlApuUnlock();
  }
  assert(delays == 250 && "only sample consumption can rearm a stalled producer");
  consume = true; dsp.sampleRead = 32;
  RtlApuLock(); RtlApuUnlock();
  assert(delays == 340 && dsp.sampleRead == 2912 && !locked);
  /* Both global and host policy bypass pacing, including recursive unlocks. */
  dsp.sampleRead = 0; consume = false;
  g_audio_fast_forward = true;
  RtlApuLock(); RtlApuUnlock(); assert(delays == 340 && !locked);
  g_audio_fast_forward = false; g_rtl_speculative_frame = true;
  RtlApuLock(); RtlApuUnlock(); assert(delays == 340 && !locked);
  g_rtl_speculative_frame = false;
  RtlAudioSetProducerWait(0);
  RtlApuLock(); RtlApuUnlock(); assert(delays == 340 && !locked);
  return 0;
}
'''
    run_c(tmp_path, source)


def test_opt_in_hook_is_outside_outermost_mutex_and_not_recursive(tmp_path):
    host = (ROOT / "runner/src/desktop/host_main.c").read_text(encoding="utf-8")
    rtl = (ROOT / "runner/src/common_rtl.c").read_text(encoding="utf-8")
    source = STUBS + function(rtl, "RtlAudioSetProducerWait")
    source += function(rtl, "RtlAudioWaitForConsumer")
    source += function(host, "RtlApuLock") + function(host, "RtlApuUnlock")
    source += r'''
static unsigned calls;
static void inspect_consumer(void) {
  assert(!locked && !g_apu_lock_depth);
  ++calls;
  RtlApuLock(); RtlApuUnlock(); /* Reentry must skip the hook. */
}
int main(void) {
  g_audio_producer_active = true;
  RtlApuLock(); RtlApuUnlock(); assert(!calls); /* Default opt-out. */
  RtlAudioSetProducerWait(inspect_consumer);
  RtlApuLock(); RtlApuLock();
  RtlApuUnlock(); assert(!calls && locked == 1);
  RtlApuUnlock(); assert(calls == 1 && !locked);
  g_audio_producer_active = false;
  RtlApuLock(); RtlApuUnlock(); assert(calls == 1 && !locked);
  return 0;
}
'''
    run_c(tmp_path, source)


def test_long_frame_streams_without_fifo_loss_and_keeps_guest_time(tmp_path):
    host = (ROOT / "runner/src/desktop/host_main.c").read_text(encoding="utf-8")
    rtl = (ROOT / "runner/src/common_rtl.c").read_text(encoding="utf-8")
    source = STUBS + r'''
#include <stdio.h>
#include <stdlib.h>
''' + (ROOT / "runner/src/apu_frame_clock.h").read_text(encoding="utf-8") + r'''
static struct { uint64_t master_cycles; } g_cpu;
static bool g_extended_frame_timing = true;
static RtlApuFrameClock g_apu_frame_clock;
static unsigned snes_frame_counter;
static uint64_t s_apu_boundary_ns, s_apu_boundary_calls;
static uint64_t sync_cycles, dropped, peak;
static unsigned slices;
static bool g_apu_frame_time_valid = true;
static void rtl_accumulate_apu_catchup(void) { assert(0); }
static void snes_catchupApu(void *s) { (void)s; assert(0); }
static uint64_t rtl_apu_guest_cycle(void) {
  return rtl_apu_clock_now(&g_apu_frame_clock, g_cpu.master_cycles);
}
enum { AUDIO_TRACE_PRODUCER_CPU, AUDIO_TRACE_PRODUCER_UNKNOWN };
static void audio_trace_set_producer(int p) { (void)p; }
static void audio_trace_on_guest_sync(int kind, uint64_t cycles) {
  assert(kind == 0 || kind == 1); sync_cycles = cycles;
}
uint64_t snesrecomp_host_now_ns(void) { return 0; }
static bool apu_runToGuestCycle(struct Apu *a, uint64_t target, uint32_t budget) {
  assert(locked == 1 && target >= a->portClock);
  ++slices;
  uint64_t cycles = target - a->portClock;
  if (cycles > budget) cycles = budget;
  uint32_t samples = (uint32_t)((a->portClock+cycles)/32 - a->portClock/32);
  a->portClock += cycles;
  uint32_t fill = dsp_available(a->dsp);
  if (samples > 8192-fill) { dropped += samples-(8192-fill); samples=8192-fill; }
  a->dsp->sampleWrite += samples;
  fill = dsp_available(a->dsp);
  if (fill > peak) peak = fill;
  return a->portClock == target;
}
'''
    source += function(rtl, "RtlAudioSetProducerWait")
    source += function(rtl, "RtlAudioWaitForConsumer")
    source += "\nstatic void HostWaitForAudioProducer(void);\n"
    source += function(host, "RtlApuLock") + function(host, "RtlApuUnlock")
    source += function(host, "HostWaitForAudioProducer")
    source += function(rtl, "rtl_run_apu_to_guest_cycle_locked")
    source += function(rtl, "rtl_sync_apu_to_cpu_locked")
    source += function(rtl, "rtl_sync_apu_frame_boundary")
    source += r'''
int main(void) {
  RtlAudioSetProducerWait(HostWaitForAudioProducer);
  consume = g_audio_producer_active = true;
  /* Exactly 44 hardware fields: no shortening of the loader or loss of PCM. */
  g_cpu.master_cycles = 44*RTL_MASTER_CYCLES_PER_FRAME;
  rtl_sync_apu_frame_boundary();
  assert(!locked && !g_apu_lock_depth && !dropped && slices == 44);
  assert(apu.portClock == 44*RTL_APU_CYCLES_PER_FRAME);
  assert(sync_cycles == apu.portClock && g_apu_frame_clock.last_duration == sync_cycles);
  assert(dsp.sampleWrite == 44*534 && peak <= HOST_AUDIO_HIGH_WATER+534);
  assert(dsp.sampleRead && delays && dsp_available(&dsp) <= HOST_AUDIO_HIGH_WATER);
  /* A port read can require the same long interval before the boundary. */
  apu.portClock = dsp.sampleRead = dsp.sampleWrite = 0;
  g_apu_frame_clock = (RtlApuFrameClock){0};
  slices = delays = ticks = 0; peak = dropped = 0;
  RtlApuLock(); rtl_sync_apu_to_cpu_locked(); RtlApuUnlock();
  assert(!locked && !dropped && slices == 44);
  assert(sync_cycles == 44*RTL_APU_CYCLES_PER_FRAME && dsp.sampleWrite == 44*534);
  /* The same guest interval with a stopped consumer remains bounded. */
  apu.portClock = dsp.sampleRead = dsp.sampleWrite = 0;
  g_apu_frame_clock = (RtlApuFrameClock){0};
  consume = false; delays = ticks = 0; g_audio_consumer_stalled = false;
  rtl_sync_apu_frame_boundary();
  assert(!locked && apu.portClock == 44*RTL_APU_CYCLES_PER_FRAME && delays == 250);
  /* A pathological target still cannot evade the original 1<<20 cycle cap. */
  apu.portClock = 0; dsp.sampleRead = dsp.sampleWrite = 0;
  g_apu_frame_clock = (RtlApuFrameClock){0};
  g_cpu.master_cycles = 100*RTL_MASTER_CYCLES_PER_FRAME;
  g_audio_producer_active = false;
  rtl_sync_apu_frame_boundary();
  assert(!locked && apu.portClock == (1u<<20) && sync_cycles == (1u<<20));
  return 0;
}
'''
    run_c(tmp_path, source)
