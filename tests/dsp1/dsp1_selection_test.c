#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp1.h"

static int check(int condition, const char *message) {
  if (condition) return 0;
  fprintf(stderr, "FAIL: %s\n", message);
  return 1;
}

/* Run in a fixture directory containing a synthetic 8192-byte dsp1b.rom.
 * No synthetic firmware instructions are executed: only loader/selection
 * behavior is checked. The real firmware differential suite remains separate.
 */
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  int expect_firmware = strcmp(argv[1], "present") == 0;
  if (expect_firmware) {
    FILE *firmware = fopen("dsp1b.rom", "wb");
    if (!firmware) return 2;
    for (unsigned i = 0; i < 8192; i++) {
      if (fputc(0, firmware) == EOF) return 2;
    }
    if (fclose(firmware)) return 2;
  }
  int fails = 0;
  Dsp1 *d = dsp1_create();
  if (!d) return 1;
  int hle = strcmp(dsp1_build_implementation(), "HLE") == 0;
  fails += check(hle == SNESRECOMP_DSP1_HLE, "build metadata identifies compiled selection");
  fails += check(dsp1_hle_active(d) == hle, "creation selects compiled implementation");
  int loaded = dsp1_load_firmware(d, "missing.sfc");
  fails += check(loaded == (!hle && expect_firmware), "firmware loading is LLE-only");
  fails += check(dsp1_firmware_loaded(d) == loaded, "firmware metadata matches loader");
  fails += check(dsp1_hle_active(d) == hle, "firmware presence cannot select backend");
  dsp1_reset(d);
  fails += check(dsp1_hle_active(d) == hle, "reset retains compiled selection");
  fails += check(dsp1_firmware_loaded(d) == loaded, "reset retains LLE firmware");
  (void)dsp1_load_firmware(d, "missing.sfc");
  fails += check(dsp1_hle_active(d) == hle, "reloading cannot switch backend");
  dsp1_destroy(d);
  if (fails) return 1;
  puts("dsp1_selection_test: PASS");
  return 0;
}
