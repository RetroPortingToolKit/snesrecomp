/* Exercise the real retained ring and export code. Interpose only allocation
 * and I/O failures; production code has no test hooks or runtime selector. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_trace.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "ppu_dma_trace.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "snes/superfx.h"

static int fail_alloc, fail_open, fail_write, fail_close, inject_event;
static int allocations, releases, closes, lock_depth;
static void *snapshot;
static void *test_malloc(size_t size) {
  if (size != sizeof(AudioTracePortEvent) * AUDIO_TRACE_PORT_RING) return malloc(size);
  ++allocations;
  assert(!lock_depth && !snapshot);
  if (fail_alloc) return NULL;
  snapshot = malloc(size);
  assert(snapshot);
  return snapshot;
}
static void test_free(void *pointer) {
  if (pointer == snapshot && snapshot) { ++releases; snapshot = NULL; }
  free(pointer);
}
static FILE *test_fopen(const char *path, const char *mode) {
  return fail_open ? NULL : fopen(path, mode);
}
static int test_ferror(FILE *file) { return fail_write || ferror(file); }
static int test_fclose(FILE *file) {
  ++closes;
  int result = fclose(file);
  return fail_close ? EOF : result;
}
static int test_fprintf(FILE *file, const char *format, ...) {
  /* Advance the real producer after snapshot acquisition, while text is being
   * formatted. The exported rows must still belong to the original instant. */
  if (inject_event && format[0]=='%' && format[1]=='u' && format[2]=='\t') {
    inject_event = 0;
    RtlApuLock();
    audio_trace_on_cpu_port_write(3, 0xab);
    RtlApuUnlock();
  }
  va_list args;va_start(args, format);
  int result = vfprintf(file, format, args);
  va_end(args);
  return result;
}
#define malloc test_malloc
#define free test_free
#define fopen test_fopen
#define ferror test_ferror
#define fclose test_fclose
#define fprintf test_fprintf
#include "../../runner/src/state_dump.c"
#undef malloc
#undef free
#undef fopen
#undef ferror
#undef fclose
#undef fprintf

uint8_t g_ram[0x20000];
Ppu *g_ppu;
Snes *g_snes;
CpuState g_cpu;
uint8_t g_snesrecomp_last_hdmaen;
uint8_t g_snes_ppu_dbg_layer_mask;
int snes_frame_counter;
void RtlApuLock(void) { assert(lock_depth==0);++lock_depth; }
void RtlApuUnlock(void) { assert(lock_depth==1);--lock_depth; }
int ppu_wlog_collect(uint32_t frame, PpuWlogEntry *out, int cap, int *lost) {
  (void)frame;(void)out;(void)cap;if(lost)*lost=0;return 0;
}
int ppudma_dma_collect(uint32_t frame, PpuDmaInfo *out, int cap, int *lost) {
  (void)frame;(void)out;(void)cap;if(lost)*lost=0;return 0;
}
int superfx_job_log(SuperFxJob *out, int cap) { (void)out;(void)cap;return 0; }
static void beam(uint16_t *vpos, uint16_t *hpos) {
  *vpos=(uint16_t)(snes_frame_counter%262);
  *hpos=(uint16_t)(snes_frame_counter*4%1364);
}
static void check_rows(const char *path, unsigned first, unsigned count) {
  FILE *file=fopen(path,"r");assert(file);
  char line[256];assert(fgets(line,sizeof(line),file));
  assert(strcmp(line,"# frame\tscanline\thclock\tevent\tport\tvalue\tsample\n")==0);
  for(unsigned row=0;row<count;++row) {
    unsigned frame,scanline,hclock,port,value;unsigned long long sample;char event[16];
    assert(fgets(line,sizeof(line),file));
    assert(sscanf(line,"%u\t%u\t%u\t%15s\t%u\t%x\t%llu",&frame,&scanline,&hclock,event,&port,&value,&sample)==7);
    unsigned expected=first+row;
    assert(frame==expected && scanline==expected%262 && hclock==expected*4%1364);
    assert(strcmp(event,"cpu_wr")==0 && port==(expected&3) && value==(expected&255) && sample==0);
  }
  assert(!fgets(line,sizeof(line),file));fclose(file);
}
int main(void) {
  assert(write_apu_events("","empty")==0);
  check_rows("empty.apu.tsv",0,0);
  assert(allocations==1 && releases==1 && !snapshot && closes==1);
  audio_trace_set_beam_probe(beam);
  RtlApuLock();
  for(unsigned i=0;i<AUDIO_TRACE_PORT_RING+17;++i) {
    snes_frame_counter=(int)i;
    audio_trace_on_cpu_port_write((uint8_t)(i&3),(uint8_t)i);
  }
  RtlApuUnlock();
  inject_event=1;
  assert(write_apu_events("","wrapped")==0 && !inject_event);
  check_rows("wrapped.apu.tsv",17,AUDIO_TRACE_PORT_RING);
  assert(allocations==2 && releases==2 && !snapshot && !lock_depth);

  fail_open=1;assert(write_apu_events("","open-failure")==-1);fail_open=0;
  assert(allocations==2 && releases==2 && closes==2);
  fail_alloc=1;assert(write_apu_events("","alloc-failure")==-1);fail_alloc=0;
  assert(allocations==3 && releases==2 && !snapshot && closes==3);
  fail_write=1;assert(write_apu_events("","write-failure")==-1);fail_write=0;
  assert(allocations==4 && releases==3 && !snapshot && closes==4);
  fail_close=1;assert(write_apu_events("","close-failure")==-1);fail_close=0;
  assert(allocations==5 && releases==4 && !snapshot && closes==5);
  fail_alloc=1;assert(snes_state_dump("","public-oom",NULL,0,0,0,0)==-1);fail_alloc=0;
  assert(!snapshot && allocations==6 && releases==4 && !lock_depth);
  uint64_t total=0;audio_trace_copy_port_events(0,0,NULL,NULL,&total);
  assert(total==AUDIO_TRACE_PORT_RING+18); /* exports never consume the history */
  const char *files[]={"empty.apu.tsv","wrapped.apu.tsv","alloc-failure.apu.tsv",
    "write-failure.apu.tsv","close-failure.apu.tsv","public-oom.wram.bin",
    "public-oom.regs.json","public-oom.ppuw.tsv","public-oom.dma.tsv",
    "public-oom.apu.tsv","public-oom.info.json"};
  for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);++i)remove(files[i]);
  puts("Scene export: empty/full wrapped snapshot, producer advancement, allocation/open/write/close failure and cleanup pass");
  return 0;
}
