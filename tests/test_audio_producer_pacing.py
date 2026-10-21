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
static struct Apu { Dsp *dsp; } apu = { &dsp };
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
