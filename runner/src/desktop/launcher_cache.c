#include "launcher_cache.h"

#include "host_paths.h"

#include <stdio.h>
#include <string.h>

static void rom_cache_path(char *out, size_t max_len) {
    if (!snesrecomp_exe_dir_path("rom.cfg", out, max_len))
        snprintf(out, max_len, "rom.cfg");
}

int snesrecomp_rom_cache_read(char *path_out, size_t max_len) {
    if (!path_out || max_len == 0) return 0;
    path_out[0] = '\0';

    char cfg_path[512];
    rom_cache_path(cfg_path, sizeof(cfg_path));
    FILE *f = fopen(cfg_path, "r");
    if (!f) return 0;

    char *got = fgets(path_out, (int)max_len, f);
    fclose(f);
    if (!got) return 0;

    size_t len = strlen(path_out);
    while (len > 0 &&
           (path_out[len - 1] == '\n' || path_out[len - 1] == '\r'))
        path_out[--len] = '\0';
    return len != 0;
}

int snesrecomp_rom_cache_write(const char *rom_path) {
    if (!rom_path || rom_path[0] == '\0') return 0;

    char cfg_path[512];
    rom_cache_path(cfg_path, sizeof(cfg_path));
    FILE *f = fopen(cfg_path, "w");
    if (!f) return 0;
    int ok = fprintf(f, "%s\n", rom_path) >= 0;
    if (fclose(f) != 0) ok = 0;
    return ok;
}
