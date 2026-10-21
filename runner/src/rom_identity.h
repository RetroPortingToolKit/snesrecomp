#pragma once

/*
 * rom_identity - name a SNES ROM image and explain why it is refused.
 *
 * Players pick the wrong dump constantly: the other revision, another region,
 * a collection re-release, a sibling game, a patched copy, the ZIP it came
 * in. A digest mismatch alone tells them nothing. A port lists the dumps it
 * knows (SnesRomRevision, usually No-Intro names and digests) and every ROM
 * prompt -- the launcher, the command-line/file-picker path and mod source-ROM
 * pickers -- says which one was picked and which one is needed.
 *
 * All digests cover the image with a 512-byte copier header removed (a file
 * whose size is 512 past a 1 KiB boundary carries one).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRomRevision {
  const char *label;       /* "Mega Man X (USA) (Rev 1)" */
  const char *sha256_hex;  /* hex digests, any case; NULL/"" leaves one */
  const char *sha1_hex;    /* out. An entry matches when every digest it */
  const char *crc32_hex;   /* gives agrees, so give at least one. */
  const char *note;        /* extra advice when this dump is picked; NULL */
} SnesRomRevision;

typedef enum SnesRomVerdict {
  kSnesRom_Ok = 0,        /* the required dump */
  kSnesRom_Wrong,         /* a catalogued dump, but not the required one */
  kSnesRom_Unknown,       /* matches nothing in the catalog */
  kSnesRom_Archive,       /* a ZIP/7z/RAR/gzip archive, not a ROM */
  kSnesRom_Unreadable,    /* missing, empty or unreadable */
} SnesRomVerdict;

typedef struct SnesRomFacts {
  SnesRomVerdict verdict;
  const char *archive_kind;   /* "ZIP" etc. for kSnesRom_Archive */
  int revision;               /* catalog index it matched, or -1 */
  char sha256_hex[65];        /* lowercase; empty when unreadable/archive */
  char sha1_hex[41];
  char crc32_hex[9];          /* uppercase */
} SnesRomFacts;

/* Whether `r` matches these digests (every digest the entry gives agrees). */
int snes_rom_revision_matches(const SnesRomRevision *r, const char *sha256_hex,
                              const char *sha1_hex, const char *crc32_hex);

/* Index of the entry whose SHA-256 is `sha256_hex`, or -1. */
int snes_rom_revision_find_sha256(const SnesRomRevision *catalog, int n,
                                  const char *sha256_hex);

/* Describe a header-stripped image's internal SNES header for a player:
 * title, version, region and whether the header checksum still matches the
 * data. Returns 0 (out = "") when no plausible header is found. */
int snes_rom_describe_header(const uint8_t *image, size_t len, char *out,
                             size_t cap);

/* Examine a ROM file (raw, copier header allowed) against `required_sha256_hex`
 * (64 hex) and the catalog. When `explain` is non-NULL and the verdict is not
 * kSnesRom_Ok, writes a player-facing explanation:
 *   subject != NULL: "This is <X>. <subject> needs <Y> (SHA-256 ...)."
 *   subject == NULL: compact, for a status line: "Wrong ROM: this is <X>.
 *                    Select <Y> (SHA-256 ...)."
 * `help`, when given, is appended to the long form. Returns the verdict. */
SnesRomVerdict snes_rom_identity_check_file(
    const char *path, const SnesRomRevision *catalog, int n,
    const char *required_sha256_hex, const char *subject, const char *help,
    SnesRomFacts *facts, char *explain, size_t explain_cap);

/* Same, for an image already in memory (`raw` may carry a copier header). */
SnesRomVerdict snes_rom_identity_check_image(
    const uint8_t *raw, size_t raw_len, const SnesRomRevision *catalog, int n,
    const char *required_sha256_hex, const char *subject, const char *help,
    SnesRomFacts *facts, char *explain, size_t explain_cap);

#ifdef __cplusplus
}
#endif
