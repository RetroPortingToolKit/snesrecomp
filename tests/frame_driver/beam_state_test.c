#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../runner/src/beam_frame_driver_state.c"

static uint32_t resume;
static bool halted, staged, failed;
static uint64_t execution = 0x12345678, loaded;
static uint32_t raster=1234;
size_t ppu_rasterStateSize(void) {return sizeof raster;}
bool ppu_rasterStateValid(const void *in,size_t size) {
  uint32_t value; if(size!=sizeof value)return false;
  memcpy(&value,in,sizeof value);return value==1234;
}
bool ppu_rasterStateSave(SaveLoadInfo *s) {s->func(s,&raster,sizeof raster);return true;}
bool ppu_rasterStateLoad(SaveLoadInfo *s) {s->func(s,&raster,sizeof raster);return true;}
size_t RtlSnapshotGuestSize(uint32 *version) { *version = 10; return 8; }
void snes_beam_frame_driver_get_position(uint32_t *pc, bool *wai) { *pc=resume; *wai=halted; }
void snes_beam_frame_driver_set_position(uint32_t pc, bool wai) { resume=pc; halted=wai; }
size_t RtlExecutionStateSize(void) { return sizeof execution; }
bool RtlExecutionStateValid(const void *in, size_t size) {
  uint64_t value; if (!in || size != sizeof value) return false;
  memcpy(&value,in,sizeof value);return value==0x12345678;
}
void RtlSaveExecutionState(SaveLoadInfo *s) { s->func(s,&execution,sizeof execution); }
bool RtlLoadExecutionState(SaveLoadInfo *s) {
  s->func(s,&loaded,sizeof loaded);return staged=true;
}
void RtlApplyExecutionState(void) { assert(staged);execution=loaded;staged=false; }
void RtlStateStreamFail(SaveLoadInfo *s) { (void)s;failed=true; }
typedef struct { SaveLoadInfo base; uint8_t bytes[8+sizeof(BeamFrameState)+sizeof execution+sizeof raster]; size_t position; bool save; } Stream;
static void transfer(SaveLoadInfo *base, void *data, size_t size) {
  Stream *s=(Stream *)base;assert(s->position+size<=sizeof s->bytes);
  if(s->save)memcpy(s->bytes+s->position,data,size);
  else memcpy(data,s->bytes+s->position,size);
  s->position+=size;
}
int main(void) {
  Stream stream={0};stream.base.func=transfer;stream.position=8;stream.save=true;
  uint32_t header[2]={0x534c5452,10};memcpy(stream.bytes,header,sizeof header);
  resume=0xc04e43;halted=true;
  snes_beam_frame_driver_state_save(&stream.base);
  assert(stream.position==sizeof stream.bytes);
  assert(snes_beam_frame_driver_state_preflight(stream.bytes,sizeof stream.bytes));
  for(size_t n=0;n<sizeof stream.bytes;++n)
    assert(!snes_beam_frame_driver_state_preflight(stream.bytes,n));
  execution=0;raster=0;resume=0x808000;halted=false;
  stream.position=8;stream.save=false;
  snes_beam_frame_driver_state_load(&stream.base,10);
  assert(!failed && staged && execution==0 && resume==0xc04e43 && halted);
  snes_beam_frame_driver_state_loaded(10);
  assert(!staged && execution==0x12345678 && raster==1234);
  for(unsigned fault=0;fault<6;++fault) {
    Stream bad=stream;BeamFrameState state;memcpy(&state,bad.bytes+8,sizeof state);
    switch(fault) {
      case 0:state.magic=0;break;
      case 1:state.size--;break;
      case 2:state.resume_pc=0x2140;break;
      case 3:state.wai_halted=2;break;
      case 4:bad.bytes[8+sizeof state]=0;break;
      case 5:bad.bytes[8+sizeof state+sizeof execution]=0;break;
    }
    memcpy(bad.bytes+8,&state,sizeof state);
    assert(!snes_beam_frame_driver_state_preflight(bad.bytes,sizeof bad.bytes));
    assert(execution==0x12345678 && resume==0xc04e43 && halted);
  }
  puts("beam states: cursor and existing execution context restore; malformed chunks reject PASS");
  return 0;
}
