/* Android UI thread requests are consumed only on the emulation thread. */
#include "android_host.h"
#include <stdatomic.h>
#include "common_rtl.h"
static _Atomic uint32_t buttons;
static _Atomic int pause_request = -1, quit_request, slot_request = -1;
void snesrecomp_android_input(uint32_t value) { atomic_store(&buttons,value & 0xfff); }
void snesrecomp_android_pause(int value) {
  atomic_store(&buttons,0); atomic_store(&pause_request,!!value);
}
void snesrecomp_android_quit(void) {
  atomic_store(&buttons,0); atomic_store(&quit_request,1);
}
void snesrecomp_android_slot(int save) { atomic_store(&slot_request,!!save); }
uint32_t snesrecomp_android_buttons(void) { return atomic_load(&buttons); }
int snesrecomp_android_commands(bool *running,uint8_t *paused) {
  int pause=atomic_exchange(&pause_request,-1);
  if(pause>=0) *paused=(uint8_t)pause;
  if(atomic_load(&quit_request)) { *running=false; return 0; }
  int slot=atomic_exchange(&slot_request,-1);
  if(slot>=0) RtlSaveLoad(slot ? kSaveLoad_Save : kSaveLoad_Load,0);
  return slot==0;
}
