#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cart.h"
#include "snes.h"
#include "sha256.h"

static uint8_t *bus;
void cart_load(Cart *cart, int type, uint8_t *rom, int size, int ram_size) {
  (void)type; (void)ram_size;
  free(bus); bus = malloc(size);
  memcpy(bus, rom, size); cart->rom = bus; cart->romSize = size;
}
static int check_case(int image_size, int copier_header) {
  const int prefix = copier_header ? 512 : 0;
  uint8_t *file = calloc(1, image_size + prefix), *rom = file + prefix;
  Cart cart = {0}; Snes snes = {0}; snes.cart = &cart;
  for (int i = 0; i < image_size; ++i) rom[i] = (uint8_t)(i * 17 + (i >> 16));
  memset(rom + 0x7fc0, 0, 64);
  memcpy(rom + 0x7fc0, "COVERAGE IDENTITY    ", 21);
  rom[0x7fd5] = 0x20; rom[0x7fd6] = 2; rom[0x7fd7] = 12;
  rom[0x7fd8] = 3; rom[0x7fd9] = 1;
  rom[0x7fdc] = 0x34; rom[0x7fdd] = 0x12;
  rom[0x7fde] = 0xcb; rom[0x7fdf] = 0xed;
  rom[0x7ffc] = 0; rom[0x7ffd] = 0x80; rom[0] = 0x78;
  uint8_t expected[32], actual[32], padded[32];
  sha256_compute(rom, image_size, expected);
  int ok = snes_loadRom(&snes, file, image_size + prefix);
  ok &= cart.romImageSize == (uint32_t)image_size;
  sha256_compute(cart.rom, cart.romImageSize, actual);
  sha256_compute(cart.rom, cart.romSize, padded);
  ok &= memcmp(actual, expected, 32) == 0;
  if (image_size == 3 * 1024 * 1024)
    ok &= cart.romSize == 4 * 1024 * 1024 && memcmp(actual, padded, 32) != 0;
  else ok &= cart.romSize == (uint32_t)image_size;
  free(file);
  if (!ok) fprintf(stderr, "FAIL: image=%d copier_header=%d\n", image_size, copier_header);
  return !ok;
}
int main(void) {
  int fails = check_case(3 * 1024 * 1024, 0) + check_case(3 * 1024 * 1024, 1)
            + check_case(512 * 1024, 0) + check_case(512 * 1024, 1);
  free(bus);
  if (!fails) puts("rom_image_identity_test: PASS");
  return fails != 0;
}
