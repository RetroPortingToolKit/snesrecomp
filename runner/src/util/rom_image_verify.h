#pragma once

#include <stddef.h>
#include <stdint.h>

int snesrecomp_rom_is_readable(const char *path);
int snesrecomp_rom_verify_crc32(const char *path, uint32_t expected_crc);
int snesrecomp_rom_verify_sha256(const char *path,
                                 const uint8_t *expected_sha256);
int snesrecomp_rom_match_sha256(const char *path,
                                const uint8_t (*hashes)[32],
                                size_t n_hashes);
