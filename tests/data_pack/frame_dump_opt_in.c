#include "framedump.h"
int main(void) {
  /* Must link without game/PPU globals and remain inert before opt-in. */
  FrameDump_Present(0, 0, 0, 0, 0);
  FrameDump_Ppu(0, 0);
  return 0;
}
