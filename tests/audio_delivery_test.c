/* Exercise the real consumer and DSP FIFO without a device or wall clock.
 * Whole-program LTO omits unrelated host entry points; run audio_delivery/run.ps1. */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../runner/src/common_rtl.c"
/* This test has one thread; only the production host supplies a real mutex. */
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static Dsp queue;
Snes *g_snes;
static int16_t output[1600];
static void produce(unsigned count) {
  assert(dsp_available(&queue)+count <= DSP_SAMPLE_RING);
  for(unsigned i=0;i<count;++i) {
    unsigned at=queue.sampleWrite++ & (DSP_SAMPLE_RING-1);
    queue.sampleBuffer[at*2]=10000;
    queue.sampleBuffer[at*2+1]=-10000;
  }
}
static AudioTraceStats stats(void) {
  AudioTraceStats result;audio_trace_get_stats(&result);return result;
}
static void start(void) {
  memset(&queue,0,sizeof(queue));
  rtl_reset_audio_delivery();
  RtlSetAudioOutputRate(32040);
}
static void render(unsigned frames) {
  uint32_t written=queue.sampleWrite;
  rtl_render_native(&queue,output,frames);
  assert(queue.sampleWrite==written); /* consumer must never advance guest time */
  for(unsigned i=0;i<frames;++i) {
    assert(output[i*2]>=0 && output[i*2]<=10000);
    assert(output[i*2+1]==-output[i*2]);
  }
}
static void reset_and_rates(void) {
  const int rates[]={32040,44100,48000};
  for(unsigned rate=0;rate<3;++rate) {
    start();RtlSetAudioOutputRate(rates[rate]);
    uint64_t underflows=stats().output_underflows;
    int block=rates[rate]/60;
    /* Cold start and replacement queues need four producer blocks. */
    for(int i=0;i<3;++i) {
      produce(534);render(block);assert(queue.sampleRead==0);
    }
    produce(534);render(block);assert(queue.sampleRead>0);
    for(int i=0;i<600;++i) { produce(534);render(block); }
    assert(stats().output_underflows==underflows);
    assert(dsp_available(&queue)>1000 && dsp_available(&queue)<3000);
    assert(abs((int)queue.sampleRead-601*534)<534);
    /* A load replaces the FIFO with a short queue while the old servo was
     * already running. Do not repeatedly drain the first arriving samples. */
    queue.sampleRead=queue.sampleWrite=0;rtl_reset_audio_delivery();
    for(int i=0;i<3;++i) { produce(534);render(block);assert(queue.sampleRead==0); }
    produce(534);render(block);
    for(int i=0;i<120;++i) { produce(534);render(block); }
    assert(stats().output_underflows==underflows);
    assert(output[(block-1)*2]==10000);
  }
}
static void short_callback_recovery(void) {
  start();produce(2136);render(534);render(534);
  uint64_t underflows=stats().output_underflows;
  int16_t previous=output[1066];
  /* Exhaust a healthy queue with callbacks shorter than the fade. There
   * must be one continuous ramp, not a restarted ramp at each callback. */
  for(int call=0;call<400;++call) {
    render(5);
    for(int i=0;i<5;++i) {
      assert(abs(output[i*2]-previous)<=157);previous=output[i*2];
    }
  }
  assert(stats().output_underflows==underflows+1 && previous==0);
  uint32_t read=queue.sampleRead;
  for(int i=0;i<3;++i) { produce(534);render(5);assert(queue.sampleRead==read); }
  produce(534);
  for(int call=0;call<20;++call) {
    render(5);
    for(int i=0;i<5;++i) {
      assert(abs(output[i*2]-previous)<=157);previous=output[i*2];
    }
  }
  assert(previous==10000 && stats().output_underflows==underflows+1);
  assert(stats().output_priming>0);
}
static void turbo_stage_entry_recovery(void) {
  static Snes machine;
  static Apu apu;
  machine.apu=&apu;apu.dsp=&queue;g_snes=&machine;
  const int rates[]={32040,44100,48000};
  for(unsigned rate=0;rate<3;++rate) {
    start();RtlSetAudioOutputRate(rates[rate]);
    g_audio_fast_forward=false;g_audio_recovery_frames=0;
    int block=rates[rate]/60;
    produce(2136);render(block);
    RtlAudioSetFastForward(true);
    /* A stage upload or callback starvation puts delivery back into priming
     * while turbo has accumulated audio. Recovery must leave enough samples
     * for the real consumer to start, rather than repeatedly trimming them. */
    rtl_reset_audio_delivery();produce(4000);
    RtlAudioSetFastForward(false);render(block);
    for(int frame=0;frame<60;++frame) {
      produce(534);RtlAudioSetFastForward(false);render(block);
      if(frame>=8) {
        assert(!s_render_priming);
        assert(output[(block-1)*2]==10000);
      }
    }
    assert(g_audio_recovery_frames==0);
  }
}
int main(void) {
  reset_and_rates();short_callback_recovery();turbo_stage_entry_recovery();
  puts("audio delivery: reset/start priming, rates, starvation ramps, turbo recovery and producer isolation passed");
  return 0;
}
