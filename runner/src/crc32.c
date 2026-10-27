#include "crc32.h"

#ifndef SNESRECOMP_CRC32_SLICES
#define SNESRECOMP_CRC32_SLICES 1
#endif
#if SNESRECOMP_CRC32_SLICES == 8
#include "crc32_slices.inc"
#elif SNESRECOMP_CRC32_SLICES == 1
static uint32_t s_table[256];
static int      s_table_ready = 0;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_table[i] = c;
    }
    s_table_ready = 1;
}
#else
#error SNESRECOMP_CRC32_SLICES must be 1 or 8
#endif

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
#if SNESRECOMP_CRC32_SLICES == 8
    /* Byte assembly is endian/alignment independent; compilers can use safe
     * native loads. This is IEEE CRC-32, not the x86 CRC32C instruction. */
    while (len >= 8) {
        crc ^= (uint32_t)data[0] | (uint32_t)data[1] << 8 |
               (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
        crc = s_slices[7][crc & 255] ^ s_slices[6][(crc >> 8) & 255] ^
              s_slices[5][(crc >> 16) & 255] ^ s_slices[4][crc >> 24] ^
              s_slices[3][data[4]] ^ s_slices[2][data[5]] ^
              s_slices[1][data[6]] ^ s_slices[0][data[7]];
        data += 8; len -= 8;
    }
    while (len--) crc = s_slices[0][(crc ^ *data++) & 255] ^ (crc >> 8);
#else
    if (!s_table_ready) crc32_init_table();
    for (size_t i = 0; i < len; i++)
        crc = s_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
#endif
    return crc;
}

uint32_t crc32_compute(const uint8_t *data, size_t len) {
    return crc32_final(crc32_update(CRC32_INIT, data, len));
}
