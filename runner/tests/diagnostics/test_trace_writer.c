#include "diagnostics/trace_writer.h"
#include "desktop/sdl_compat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static SDL_atomic_t gated, entered, released;
void SnesTraceWriterTestGate(void) {
  if(!SDL_AtomicGet(&gated)) return;
  SDL_AtomicSet(&entered,1);
  while(!SDL_AtomicGet(&released)) SDL_Delay(1);
}
static void row(char out[17],unsigned n) { snprintf(out,17,"%015u\n",n); }
static void check_file(const char *path,unsigned first,unsigned last) {
  FILE *file=fopen(path,"rb");assert(file);
  char line[80],expected[17];
  assert(fgets(line,sizeof(line),file) && !strcmp(line,"h\n"));
  for(unsigned n=first;n<=last;++n) {
    row(expected,n);
    assert(fgets(line,sizeof(line),file) && !strcmp(line,expected));
  }
  assert(fgetc(file)==EOF && !ferror(file));fclose(file);
}
int main(void) {
  assert(snesrecomp_sdl_init(0));
  SnesTraceWriter *writer=SnesTraceWriterOpen("rotation/trace","h\n",50,2);
  assert(writer);
  for(unsigned n=1;n<=7;++n) {
    char text[17];row(text,n);
    assert(SnesTraceWriterWrite(writer,text,16));
  }
  assert(!SnesTraceWriterClose(writer));
  FILE *old=fopen("rotation/trace-001.csv","rb");assert(!old);
  check_file("rotation/trace-002.csv",4,6);
  check_file("rotation/trace-003.csv",7,7);

  writer=SnesTraceWriterOpen("wrapping/trace","h\n",32*1024*1024,2);assert(writer);
  for(unsigned n=1;n<=300000;++n) {
    char text[17];row(text,n);
    assert(SnesTraceWriterWrite(writer,text,16));
    if(n%2048==0) SDL_Delay(1);
  }
  assert(!SnesTraceWriterClose(writer));
  check_file("wrapping/trace-001.csv",1,300000);

  /* A blocked sink must neither block the producer nor grow its memory.
   * On saturation, accepted rows remain a complete ordered prefix. */
  SDL_AtomicSet(&gated,1);
  writer=SnesTraceWriterOpen("stalled/trace","h\n",16*1024*1024,2);assert(writer);
  uint32_t deadline=(uint32_t)SDL_GetTicks();
  while(!SDL_AtomicGet(&entered) && (uint32_t)SDL_GetTicks()-deadline<3000) SDL_Delay(1);
  assert(SDL_AtomicGet(&entered));
  uint64_t start=SDL_GetPerformanceCounter();
  unsigned accepted=0;
  for(unsigned n=1;n<500000;++n) {
    char text[17];row(text,n);
    if(!SnesTraceWriterWrite(writer,text,16)) break;
    ++accepted;
  }
  double elapsed=(double)(SDL_GetPerformanceCounter()-start)/SDL_GetPerformanceFrequency();
  assert(accepted>0 && accepted<499999 && elapsed<2.0);
  assert(strstr(SnesTraceWriterError(writer),"queue full"));
  assert(!SnesTraceWriterWrite(writer,"after failure\n",14));
  SDL_AtomicSet(&released,1);
  assert(strstr(SnesTraceWriterClose(writer),"queue full"));
  check_file("stalled/trace-001.csv",1,accepted);

  SDL_AtomicSet(&gated,0);
  writer=SnesTraceWriterOpen("missing/parent/trace","h\n",1024,2);assert(writer);
  deadline=(uint32_t)SDL_GetTicks();
  while(!SnesTraceWriterError(writer) && (uint32_t)SDL_GetTicks()-deadline<3000) SDL_Delay(1);
  assert(SnesTraceWriterError(writer));
  assert(!SnesTraceWriterWrite(writer,"x\n",2));
  assert(SnesTraceWriterClose(writer));
  SDL_Quit();
  printf("PASS rotation, ordered drain, blocked sink (%u rows in %.3f ms), I/O failure\n",accepted,elapsed*1000);
  return 0;
}
