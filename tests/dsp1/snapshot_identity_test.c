#include <stdio.h>
#include "snapshot_identity.h"

int main(void) {
  if (rtl_snapshot_magic(false) != 0x52544c53u) return 1;
  uint32_t selected = rtl_snapshot_magic(true);
  uint32_t other = SNESRECOMP_DSP1_HLE ? 0x31504c53u : 0x31504853u;
  if (selected == other || selected == rtl_snapshot_magic(false)) return 1;
  if (selected != (SNESRECOMP_DSP1_HLE ? 0x31504853u : 0x31504c53u)) return 1;
  puts("snapshot_identity_test: PASS");
  return 0;
}
