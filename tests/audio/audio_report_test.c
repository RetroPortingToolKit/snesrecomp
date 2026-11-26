/* Exercise the actual shared window/formatter without a game or audio device. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "audio_report.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { CAPACITY=160, MESSAGE=192 };
static char ring[CAPACITY][MESSAGE];
static unsigned written;
void host_report_breadcrumb(const char *fmt, ...) {
  va_list args;va_start(args,fmt);
  int n=vsnprintf(ring[written++%CAPACITY],MESSAGE,fmt,args);va_end(args);
  assert(n>=0 && n<MESSAGE); /* New records must fit the existing ring. */
}
static int contains(const char *text) {
  for(unsigned i=0;i<written && i<CAPACITY;++i)
    if(strstr(ring[i],text))return 1;
  return 0;
}
static void clear(void) {written=0;memset(ring,0,sizeof ring);}
static SnesDesktopHostFrameStats frame(unsigned f,double seconds,double guest) {
  SnesDesktopHostFrameStats s={.frame=f,.run_seconds=seconds,
      .guest_seconds=guest,.audio_output_rate=32000};return s;
}
int main(void) {
  AudioTraceStats audio={.produced=100,.consumed=90};
  SnesAudioReportContext ctx={"windows","WASAPI","vulkan","direct3d11","Headset"};
  SnesDesktopHostFrameStats s=frame(1,.1,.025);
  assert(!snes_audio_report_note(0,&s,1,0));assert(!written);
  assert(snes_audio_report_note(1,&s,1,0)==3);snes_audio_report_emit(&audio,&ctx);
  assert(contains("guest_ms=25") && contains("valid=0"));
  assert(contains("requested=vulkan actual=direct3d11") && contains("device=Headset"));
  clear();s=frame(2,.2,.010);assert(!snes_audio_report_note(1,&s,1,0));
  s=frame(121,2.1,.030);assert(snes_audio_report_note(1,&s,1,0)==1);
  audio.produced=65000;audio.consumed=64000;audio.output_underflows=2;
  audio.output_missing_frames=400;audio.dropped=8;snes_audio_report_emit(&audio,NULL);
  assert(contains("seconds=2 frames=120 fps=60 guest_ms=30 gap_ms=1900"));
  assert(contains("valid=1 uf=2 miss=400 drop=8") && contains("includes pauses"));
  clear();s=frame(122,2.2,.040);assert(snes_audio_report_note(1,&s,2,0)==3);
  snes_audio_report_emit(&audio,&ctx);
  assert(contains("reason=state-reset") && contains("frames=0 fps=0 guest_ms=40") && contains("valid=0"));
  clear();s=frame(1,2.3,.020);assert(snes_audio_report_note(1,&s,2,0)==3);
  snes_audio_report_emit(&audio,NULL);assert(contains("reason=state-reset"));
  clear();s=frame(2,.1,.020);assert(snes_audio_report_note(1,&s,2,0)==3);
  snes_audio_report_emit(&audio,NULL);assert(contains("reason=state-reset"));
  clear();s=frame(122,2.1,.010);assert(snes_audio_report_note(1,&s,2,0)==1);
  ++audio.produced;++audio.consumed;audio.output_underflows=1;
  snes_audio_report_emit(&audio,NULL);
  assert(contains("reason=counter-reset") && contains("valid=0"));
  clear();s=frame(242,4.2,.010);assert(snes_audio_report_note(1,&s,2,0)==1);
  audio=(AudioTraceStats){0};snes_audio_report_emit(&audio,NULL);
  assert(contains("reason=counter-reset") && contains("valid=0 uf=0 miss=0 drop=0"));
  clear();assert(!snes_audio_report_note(0,NULL,2,1));assert(!written);
  audio.output_underflows=99;s=frame(243,4.3,.015);
  assert(snes_audio_report_note(1,&s,2,0)==3);snes_audio_report_emit(&audio,&ctx);
  assert(contains("reason=start") && contains("valid=0"));
  clear();assert(snes_audio_report_note(1,NULL,2,1)==3);snes_audio_report_emit(&audio,&ctx);
  assert(contains("reason=final") && contains("device=Headset"));
  assert(!snes_audio_report_note(1,NULL,2,1));
  assert(!snes_audio_report_note(0,NULL,2,0));clear();
  assert(snes_audio_report_note(1,NULL,3,1)==3);snes_audio_report_emit(&audio,&ctx);
  assert(contains("frames=0 fps=0")); /* Shutdown before the first frame. */
  assert(!snes_audio_report_note(0,NULL,3,0));clear();
  s=frame(UINT_MAX,0,.001);assert(snes_audio_report_note(1,&s,3,0)==3);
  audio=(AudioTraceStats){.produced=UINT64_MAX,.consumed=UINT64_MAX,
      .output_underflows=UINT64_MAX,.output_missing_frames=UINT64_MAX,
      .dropped=UINT64_MAX,.occupancy_current=UINT_MAX};
  char long_device[110];memset(long_device,'A',95);
  memcpy(long_device+95,"\xe3\x81\x82tail",8);long_device[103]=0;
  ctx.device=long_device;snes_audio_report_emit(&audio,&ctx);
  assert(contains("18446744073709551615"));
  assert(!contains("\xe3")); /* The 96-byte name bound cannot split UTF-8. */
  for(unsigned i=0;i<1000;++i) {
    s=frame(i,2.0*i,.001);unsigned flags=snes_audio_report_note(1,&s,3,0);
    if(flags)snes_audio_report_emit(&audio,NULL);
  }
  assert(written>CAPACITY); /* Storage remains the existing bounded ring. */
  assert(!snes_audio_report_note(0,NULL,3,0));clear();
  s=frame(1,NAN,.001);assert(!snes_audio_report_note(1,&s,3,0));assert(!written);
  puts("audio report: On/Off, first/window/final, rewind/generation/counter reset and bounds passed");
  return 0;
}
