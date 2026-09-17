#pragma once

/* Compatibility umbrella: callers that historically included launcher.h for
 * executable-relative path helpers continue to receive those declarations. */
#include "host_paths.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolve a SNES ROM in this order:
 *   1. argv[1], when present and not a flag;
 *   2. the cached path in <exe_dir>/rom.cfg;
 *   3. a native file picker.
 *
 * Linux and other Unix-like hosts try zenity, kdialog, qarma, then osascript.
 * A positional ROM is made absolute before it is cached. Explicit positional
 * overrides warn and continue on a verification mismatch; cached and picked
 * ROMs re-prompt until they pass.
 *
 * A 512-byte SMC copier header is detected by file size and excluded from all
 * verification hashes.
 */
int snesrecomp_launcher_resolve_rom(int argc, char **argv,
                                    char *out_path, size_t max_len,
                                    uint32_t expected_crc);

/*
 * SHA-256 equivalent of snesrecomp_launcher_resolve_rom.
 * Pass NULL for expected_sha256 to skip verification.
 */
int snesrecomp_launcher_resolve_rom_sha256(int argc, char **argv,
                                           char *out_path, size_t max_len,
                                           const uint8_t *expected_sha256);

/*
 * Permissive multi-hash variant. Known ROMs load silently; any other readable
 * ROM loads with a warning so ROM hacks and other regions remain usable.
 * Pass n_hashes == 0 to accept any readable ROM silently.
 */
int snesrecomp_launcher_resolve_rom_sha256_multi(int argc, char **argv,
                                                 char *out_path, size_t max_len,
                                                 const uint8_t (*hashes)[32],
                                                 size_t n_hashes);

#ifdef __cplusplus
}
#endif
