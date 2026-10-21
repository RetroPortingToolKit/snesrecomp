#include "diagnostics/replay_writer.h"
#include "desktop/sdl_compat.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_atomic_t gated,entered,released;
void SnesReplayWriterTestGate(void) {
  if(!SDL_AtomicGet(&gated))return;
  SDL_AtomicSet(&entered,1);
  while(!SDL_AtomicGet(&released))SDL_Delay(1);
}
static uint32_t hash(const void *p,size_t n) {
  const unsigned char *b=p;uint32_t h=2166136261u;
  while(n--)h=(h^*b++)*16777619u;return h;
}
static void text_file(const char *path,const char *expected) {
  FILE *f=fopen(path,"rb");assert(f);char actual[512];
  size_t n=fread(actual,1,sizeof(actual),f);
  assert(n==strlen(expected) && !memcmp(actual,expected,n));fclose(f);
}
int main(void) {
  assert(snesrecomp_sdl_init(0));
  FILE *config=fopen("config.ini","wb");assert(config);fputs("setting=value\n",config);fclose(config);
  enum { SIZE=325707, FRAMES=180 };
  unsigned char *snapshot=malloc(SIZE);assert(snapshot);
  for(unsigned i=0;i<SIZE;++i)snapshot[i]=(unsigned char)(i*13);
  SnesReplayWriter *w=SnesReplayWriterCreate();assert(w);
  assert(SnesReplayWriterBegin(w,"old.replay","header\n","config.ini","absent.toml","mods\n"));
  assert(SnesReplayWriterSnapshot(w,snapshot,SIZE));
  assert(SnesReplayWriterEnd(w));
  assert(SnesReplayWriterRemove(w,"old.replay"));
  assert(SnesReplayWriterRemove(w,"absent-file"));
  assert(SnesReplayWriterBegin(w,"capture.replay","header\n","config.ini","absent.toml","mods\n"));
  assert(SnesReplayWriterSnapshot(w,snapshot,SIZE));
  uint32_t digest=hash(snapshot,SIZE);
  /* More than one full ring, with binary snapshots crossing the wrap point.
   * Mutating source memory afterwards proves queued frame copies are owned. */
  for(unsigned n=0;n<FRAMES;++n) {
    char line[64];snprintf(line,sizeof(line),"I %u\n",n);
    assert(SnesReplayWriterLine(w,line));
    assert(SnesReplayWriterFrame(w,n+1,snapshot,SIZE,snapshot,7,snapshot+7,13,snapshot+20,31));
    if(n==80)assert(SnesReplayWriterSnapshot(w,snapshot,SIZE));
    SDL_Delay(2);
  }
  memset(snapshot,0,SIZE);
  assert(SnesReplayWriterEnd(w));assert(!SnesReplayWriterDestroy(w));
  assert(!fopen("old.replay","rb"));
  text_file("capture.replay.config.ini","setting=value\n");text_file("capture.replay.mods.txt","mods\n");
  FILE *state=fopen("capture.replay.sav","rb");assert(state);
  for(unsigned copy=0;copy<2;++copy)for(unsigned i=0;i<SIZE;++i)assert(fgetc(state)==(unsigned char)(i*13));
  assert(fgetc(state)==EOF);fclose(state);
  for(unsigned i=0;i<SIZE;++i)snapshot[i]=(unsigned char)(i*13);
  FILE *trace=fopen("capture.replay","rb");assert(trace);char line[256],expected[256];
  assert(fgets(line,sizeof(line),trace) && !strcmp(line,"header\n"));
  snprintf(expected,sizeof(expected),"S 0 %u %08x\n",SIZE,digest);
  assert(fgets(line,sizeof(line),trace) && !strcmp(line,expected));
  for(unsigned n=0;n<FRAMES;++n) {
    snprintf(expected,sizeof(expected),"I %u\n",n);
    assert(fgets(line,sizeof(line),trace) && !strcmp(line,expected));
    snprintf(expected,sizeof(expected),"H %u %08x %08x %08x %08x\n",n+1,digest,
        hash(snapshot,7),hash(snapshot+7,13),hash(snapshot+20,31));
    assert(fgets(line,sizeof(line),trace) && !strcmp(line,expected));
    if(n==80) {
      snprintf(expected,sizeof(expected),"S %u %u %08x\n",SIZE,SIZE,digest);
      assert(fgets(line,sizeof(line),trace) && !strcmp(line,expected));
    }
  }
  assert(fgetc(trace)==EOF);fclose(trace);
  SDL_AtomicSet(&gated,1);w=SnesReplayWriterCreate();assert(w);
  uint32_t start=(uint32_t)SDL_GetTicks();
  while(!SDL_AtomicGet(&entered) && (uint32_t)SDL_GetTicks()-start<3000)SDL_Delay(1);
  assert(SDL_AtomicGet(&entered));
  assert(SnesReplayWriterBegin(w,"stalled.replay","header\n","config.ini","absent.toml","mods\n"));
  start=(uint32_t)SDL_GetTicks();unsigned accepted=0;
  while(SnesReplayWriterSnapshot(w,snapshot,SIZE))++accepted;
  assert(accepted>0 && accepted<100 && (uint32_t)SDL_GetTicks()-start<1000);
  assert(strstr(SnesReplayWriterError(w),"queue full"));
  SDL_AtomicSet(&released,1);assert(strstr(SnesReplayWriterDestroy(w),"queue full"));
  state=fopen("stalled.replay.sav","rb");assert(state);assert(!fseek(state,0,SEEK_END));
  assert(ftell(state)==(long)(accepted*SIZE));fclose(state);
  SDL_AtomicSet(&gated,0);w=SnesReplayWriterCreate();assert(w);
  assert(SnesReplayWriterBegin(w,"missing/capture.replay","header\n","config.ini","absent.toml","mods\n"));
  assert(strstr(SnesReplayWriterDestroy(w),"write failed"));
  free(snapshot);SDL_Quit();
  puts("PASS replay ordered writes, binary wrap, owned frame hashes, blocked sink, I/O failure");
  return 0;
}
