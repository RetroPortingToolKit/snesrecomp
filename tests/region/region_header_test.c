/* Console region selection in snes_loadRom: the header's $FFD9 destination
 * code picks NTSC or PAL. snes_set_region_override and SNESRECOMP_REGION
 * override it. The choice is made before cart_load so the coprocessors it
 * creates see it. No game ROM required. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cart.h"
#include "snes.h"

static Snes snes;
static int region_at_cart_load = -1;

void cart_load(Cart *cart, int type, uint8_t *rom, int rom_size, int ram_size) {
  (void)cart;
  (void)type;
  (void)rom;
  (void)rom_size;
  (void)ram_size;
  region_at_cart_load = snes.palTiming;
}

static int check(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
  }
  return 0;
}

static void set_region_env(const char *value) {
#ifdef _WIN32
  char buf[64];
  snprintf(buf, sizeof(buf), "SNESRECOMP_REGION=%s", value ? value : "");
  _putenv(buf);
#else
  if (value)
    setenv("SNESRECOMP_REGION", value, 1);
  else
    unsetenv("SNESRECOMP_REGION");
#endif
}

/* A plain LoROM header with the given destination code and a matching
 * checksum pair. Returns the loaded machine's PAL flag. */
static int load_with_destination(uint8_t destination) {
  static uint8_t rom[0x8000];
  const size_t header = 0x7fc0;
  Cart cart;
  memset(&snes, 0, sizeof(snes));
  memset(&cart, 0, sizeof(cart));
  memset(rom, 0, sizeof(rom));
  snes.cart = &cart;
  memcpy(rom + header, "REGION TEST          ", 21);
  rom[header + 0x15] = 0x20;        /* LoROM, SlowROM */
  rom[header + 0x16] = 0x00;        /* ROM only */
  rom[header + 0x17] = 5;           /* 32 KiB */
  rom[header + 0x19] = destination;
  rom[header + 0x1a] = 0x01;
  rom[header + 0x1c] = 0x00;        /* complement */
  rom[header + 0x1d] = 0x00;
  rom[header + 0x1e] = 0xff;        /* checksum */
  rom[header + 0x1f] = 0xff;
  rom[header + 0x3c] = 0x00;        /* reset vector $8000 */
  rom[header + 0x3d] = 0x80;
  rom[0] = 0x78;                    /* sei */
  region_at_cart_load = -1;
  if (!snes_loadRom(&snes, rom, sizeof(rom))) return -1;
  return snes.palTiming;
}

int main(void) {
  int fails = 0;
  set_region_env(NULL);

  /* The destination codes snes_other.c treats as PAL: $02-$0C and $11. */
  fails += check(load_with_destination(0x00) == 0, "$00 Japan is NTSC");
  fails += check(load_with_destination(0x01) == 0, "$01 North America is NTSC");
  fails += check(load_with_destination(0x02) == 1, "$02 Europe is PAL");
  fails += check(load_with_destination(0x06) == 1, "$06 France (SECAM, 50 Hz) is PAL");
  fails += check(load_with_destination(0x09) == 1, "$09 Germany is PAL");
  fails += check(load_with_destination(0x0d) == 0, "$0D Korea is NTSC");
  fails += check(load_with_destination(0x11) == 1, "$11 Australia is PAL");
  fails += check(region_at_cart_load == 1, "the region is set before cart_load");

  snes_set_region_override(kSnesRegionNtsc);
  fails += check(load_with_destination(0x02) == 0, "override NTSC beats a PAL header");
  snes_set_region_override(kSnesRegionPal);
  fails += check(load_with_destination(0x01) == 1, "override PAL beats an NTSC header");
  set_region_env("ntsc");
  fails += check(load_with_destination(0x01) == 1,
                 "an explicit override beats SNESRECOMP_REGION");
  snes_set_region_override(kSnesRegionAuto);
  fails += check(load_with_destination(0x02) == 0, "SNESRECOMP_REGION=ntsc beats the header");
  set_region_env("pal");
  fails += check(load_with_destination(0x01) == 1, "SNESRECOMP_REGION=pal beats the header");
  set_region_env("auto");
  fails += check(load_with_destination(0x02) == 1, "SNESRECOMP_REGION=auto follows the header");
  set_region_env(NULL);

  /* The helpers every timing site reads. */
  snes.palTiming = false;
  fails += check(snes_lines_per_field(&snes) == 262 &&
                 snes_master_clocks_per_field(&snes) == 357368, "NTSC field");
  snes.palTiming = true;
  fails += check(snes_lines_per_field(&snes) == 312 &&
                 snes_master_clocks_per_field(&snes) == 425568, "PAL field");
  fails += check(snes_lines_per_field(NULL) == 262, "no machine is NTSC");
  fails += check(snes_apu_cycles_per_master(&snes) * 425568.0 * 50.0 > 1025279.9 &&
                 snes_apu_cycles_per_master(&snes) * 425568.0 * 50.0 < 1025280.1,
                 "PAL APU ratio is 1025280 SPC cycles per 50 fields");
  snes.palTiming = false;
  fails += check(snes_apu_cycles_per_master(&snes) ==
                     (32040 * 32) / (1364 * 262 * 60.0),
                 "NTSC APU ratio is unchanged");

  if (fails) return 1;
  puts("region_header_test: PASS");
  return 0;
}
