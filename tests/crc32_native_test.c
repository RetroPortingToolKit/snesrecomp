#include "crc32.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint8_t input[131089];
static uint32_t reference(uint32_t crc, const uint8_t *data, size_t n) {
  while (n--) {
    crc ^= *data++;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320u : 0);
  }
  return crc;
}
int main(int argc, char **argv) {
  uint32_t random = 0x8371abc9;
  for (unsigned i = 0; i < sizeof(input); ++i) {
    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
    input[i] = (uint8_t)random;
  }
  if (crc32_compute(NULL, 0) || crc32_compute((const uint8_t *)"123456789", 9) != 0xcbf43926u) return 1;
  const size_t lengths[] = {0,1,2,3,4,5,6,7,8,9,15,16,17,31,32,33,63,64,65,255,256,257,4095,4096,4097,65535,65536,131072};
  const uint32_t seeds[] = {0,CRC32_INIT,0x12345678,0xdeadbeef};
  for (unsigned offset = 0; offset < 16; ++offset)
    for (unsigned l = 0; l < sizeof(lengths)/sizeof(*lengths); ++l)
      for (unsigned s = 0; s < sizeof(seeds)/sizeof(*seeds); ++s) {
        size_t length = lengths[l];
        uint32_t expected = reference(seeds[s], input + offset, length);
        if (crc32_update(seeds[s], input + offset, length) != expected) return 1;
        uint32_t crc = seeds[s];
        for (size_t position = 0; position < length;) {
          size_t n = (position % 31) + 1;
          if (n > length - position) n = length - position;
          crc = crc32_update(crc, input + offset + position, n); position += n;
        }
        if (crc != expected) return 1;
      }
  puts("IEEE CRC aligned/unaligned/tail/streaming/seed contract passed");
  if (argc == 2 && !strcmp(argv[1], "--bench")) {
    volatile uint32_t sink = 0;
    clock_t start = clock();
    for (unsigned i = 0; i < 8192; ++i) sink ^= crc32_compute(input, 131072);
    printf("crc_1gib_cpu_seconds=%.6f sink=%08x\n",(double)(clock()-start)/CLOCKS_PER_SEC,(unsigned)sink);
  }
  return 0;
}
