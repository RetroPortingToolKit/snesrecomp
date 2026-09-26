#ifndef SNESRECOMP_DATA_PACK_H
#define SNESRECOMP_DATA_PACK_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in, data-only catalog. No ROM switching, code loading or guest writes.
 * Strings/blobs returned by the catalog live until destroy. The game must
 * validate the payload's semantics before advertising it to the player. */
typedef struct SnesDataPacks SnesDataPacks;
typedef void (*SnesDataPackError)(void *user, const char *source, const char *reason);
typedef struct SnesDataPack {
  const char *id, *title, *source, *payload_format;
  const uint8_t *payload;
  size_t payload_size;
} SnesDataPack;

/* Missing directory is an empty catalog; errors in individual packs do not
 * disable valid siblings. Duplicate IDs reject ALL copies, never first-wins.
 * Limits: 128 candidates, 128 MiB/payload, 512 MiB/catalog, 20,000 ZIP entries.
 * ZIP entries are read into bounded buffers, never extracted to disk. */
SnesDataPacks *snes_data_packs_scan(const char *directory, const char *game,
    const char *payload_format, const uint8_t base_sha256[32],
    const char *const *capabilities, size_t capability_count,
    SnesDataPackError error, void *user);
size_t snes_data_packs_count(const SnesDataPacks *packs);
const SnesDataPack *snes_data_packs_get(const SnesDataPacks *packs, size_t index);
void snes_data_packs_destroy(SnesDataPacks *packs);

/* Read a relative asset from the same folder/ZIP. Caller owns *bytes (free()).
 * The caller's bound is enforced before allocation. No cross-pack fallback.
 * Intended for small assets; PCM streaming is a separate, game-owned concern. */
int snes_data_pack_read(const SnesDataPacks *packs, size_t index,
    const char *relative, size_t limit, uint8_t **bytes, size_t *size,
    SnesDataPackError error, void *user);
#ifdef __cplusplus
}
#endif
#endif
