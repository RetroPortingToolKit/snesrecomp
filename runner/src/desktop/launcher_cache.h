#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read/write the launcher ROM path cached beside the executable. */
int snesrecomp_rom_cache_read(char *path_out, size_t max_len);
int snesrecomp_rom_cache_write(const char *rom_path);

#ifdef __cplusplus
}
#endif
