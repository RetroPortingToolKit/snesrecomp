#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/dsp.h"
#ifdef SNES_COSIM
#include "cpu_state.h"
CpuState g_cpu;
#endif
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
int snes_frame_counter;

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"DSP output voices failed at line %d: %s\n",__LINE__,#x);return 1; } } while(0)

static void seed(Dsp *dsp) {
  dsp_reset(dsp);
  dsp->mute=dsp->reset=false;
  dsp->masterVolumeL=dsp->masterVolumeR=127;
  dsp->echoVolumeL=70;dsp->echoVolumeR=-55;dsp->feedbackVolume=32;
  dsp->echoWrites=true;dsp->echoBufferAdr=0x6000;
  dsp->echoDelay=dsp->echoRemain=8;dsp->firValues[7]=64;
  for(unsigned i=0;i<8;++i) {
    DspChannel *c=&dsp->channel[i];
    c->gain=c->gainValue=1500+i*50;c->directGain=true;c->adsrState=3;
    c->useNoise=true;c->echoEnable=true;
    c->volumeL=(int8_t)(i==0?55:i==6?35:0);
    c->volumeR=(int8_t)(i==0?-30:i==6?40:0);
  }
}

int main(void) {
  /* These are the pre-routing canonical stream sizes, not sizeof(Dsp).
   * Adding host fields must never silently change saved states/digests. */
  CHECK(offsetof(Dsp,sampleBuffer)-offsetof(Dsp,ram)==884);
  CHECK(sizeof(Dsp)-offsetof(Dsp,ram)==(sizeof(void*)==8?33664:33660));
  CHECK(offsetof(Dsp,output_filter)<offsetof(Dsp,ram));
  uint8_t *ram[3];Dsp *dsp[3];
  for(unsigned i=0;i<3;++i) {
    ram[i]=calloc(1,0x10000);CHECK(ram[i]);
    dsp[i]=dsp_init(ram[i]);CHECK(dsp[i]);seed(dsp[i]);
  }
  /* Native reference with only the retained voice audible, including its
   * echo. All voices still execute, preserving pitch/noise dependencies. */
  dsp[2]->channel[0].volumeL=dsp[2]->channel[0].volumeR=0;
  CHECK(dsp_set_output_voices(dsp[1],0x40));
  unsigned different=0,echoed=0;
  for(unsigned n=0;n<1024;++n) {
    for(unsigned i=0;i<3;++i)dsp_cycle(dsp[i]);
    unsigned at=n*2;
    CHECK(!memcmp(dsp[0]->ram,dsp[1]->ram,offsetof(Dsp,sampleBuffer)-offsetof(Dsp,ram)));
    CHECK(!memcmp(ram[0],ram[1],0x10000));
    CHECK(dsp[0]->sampleWrite==dsp[1]->sampleWrite);
    CHECK(dsp[1]->sampleBuffer[at]==dsp[2]->sampleBuffer[at]);
    CHECK(dsp[1]->sampleBuffer[at+1]==dsp[2]->sampleBuffer[at+1]);
    different+=dsp[0]->sampleBuffer[at]!=dsp[1]->sampleBuffer[at];
    if(n>16) {
      DspChannel *c=&dsp[1]->channel[6];
      int dry=(((c->sampleOut*c->volumeL)>>6)*dsp[1]->masterVolumeL)>>7;
      echoed+=dsp[1]->sampleBuffer[at]!=dry;
    }
  }
  CHECK(different>100);CHECK(echoed>100);
  CHECK(dsp_set_output_voices(dsp[1],0xff));
  for(unsigned n=1024;n<1100;++n) {
    dsp_cycle(dsp[0]);dsp_cycle(dsp[1]);
    CHECK(!memcmp(dsp[0]->ram,dsp[1]->ram,offsetof(Dsp,sampleBuffer)-offsetof(Dsp,ram)));
    CHECK(!memcmp(&dsp[0]->sampleBuffer[n*2],&dsp[1]->sampleBuffer[n*2],4));
  }
  CHECK(dsp_set_output_voices(dsp[1],0));
  for(unsigned n=1100;n<1200;++n) {
    dsp_cycle(dsp[0]);dsp_cycle(dsp[1]);
    CHECK(!dsp[1]->sampleBuffer[n*2] && !dsp[1]->sampleBuffer[n*2+1]);
    CHECK(!memcmp(dsp[0]->ram,dsp[1]->ram,offsetof(Dsp,sampleBuffer)-offsetof(Dsp,ram)));
    CHECK(!memcmp(ram[0],ram[1],0x10000));
  }
  dsp_suspend_output_filter(dsp[1],true);
  for(unsigned n=1200;n<1280;++n) {
    dsp_cycle(dsp[0]);dsp_cycle(dsp[1]);
    CHECK(!memcmp(&dsp[0]->sampleBuffer[n*2],&dsp[1]->sampleBuffer[n*2],4));
    CHECK(!memcmp(dsp[0]->ram,dsp[1]->ram,offsetof(Dsp,sampleBuffer)-offsetof(Dsp,ram)));
    CHECK(!memcmp(ram[0],ram[1],0x10000));
  }
  dsp_suspend_output_filter(dsp[1],false);
  dsp_cycle(dsp[1]);
  CHECK(!dsp[1]->sampleBuffer[1280*2] && !dsp[1]->sampleBuffer[1280*2+1]);
  for(unsigned i=0;i<3;++i) {dsp_free(dsp[i]);free(ram[i]);}
  puts("DSP output voices: native echo, canonical-state isolation, stream layout and ordinary-output restoration passed");
  return 0;
}
