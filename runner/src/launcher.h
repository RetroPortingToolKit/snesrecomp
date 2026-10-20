#pragma once

/* Compatibility umbrella: callers that historically included launcher.h for
 * executable-relative path helpers continue to receive those declarations. */
#include "host_paths.h"

#include "rom_identity.h"

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
 * What a ROM prompt checks and how it explains a refusal: the required
 * SHA-256 (NULL/"" skips verification), the port's catalog of known dumps,
 * the game name used in "<subject> needs ..." and an optional extra line.
 */
typedef struct SnesRomIdentityPolicy {
    const char *required_sha256_hex;
    const SnesRomRevision *catalog;
    int catalog_count;
    const char *subject;   /* NULL: "This game" */
    const char *help;      /* NULL: none */
} SnesRomIdentityPolicy;

/*
 * Catalog-aware form of snesrecomp_launcher_resolve_rom_sha256. A refused ROM
 * is named ("This is Mega Man X (USA). Mega Man X needs Mega Man X (USA)
 * (Rev 1) (SHA-256 ...)"), described by its internal header when unknown, or
 * reported as an archive/unreadable file -- on stderr and, for a picked ROM on
 * Windows, in a message box.
 */
int snesrecomp_launcher_resolve_rom_identity(int argc, char **argv,
                                             char *out_path, size_t max_len,
                                             const SnesRomIdentityPolicy *policy);

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
