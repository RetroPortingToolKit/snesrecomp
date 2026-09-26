#ifndef SNESRECOMP_CONTENT_PACK_H
#define SNESRECOMP_CONTENT_PACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Additive host catalog. IDs, never array positions, are the persistent keys.
 * Guest slots are adapter-local; the framework does not reinterpret them.
 * A manifest describes one independently supplied program/resource. */
enum { CP_ID = 48, CP_NAME = 96, CP_PATH = 1024, CP_CUPS = 32,
       CP_TRACKS = 128, CP_PACKS = 64, CP_ROM_LIMIT = 16 * 1024 * 1024 };
typedef struct CpCup { char id[CP_ID], name[CP_NAME]; unsigned slot; } CpCup;
typedef struct CpTrack {
    char id[CP_ID], name[CP_NAME], cup[CP_ID]; unsigned slot;
} CpTrack;
typedef struct CpPack {
    char id[CP_ID], name[CP_NAME], author[CP_NAME], adapter[CP_ID];
    uint8_t source_hash[32], target_hash[32];
    uint8_t alternate_target_hash[8][32]; unsigned alternate_target_count;
    CpCup cups[CP_CUPS]; CpTrack tracks[CP_TRACKS];
    unsigned cup_count, track_count;
} CpPack;
typedef struct CpCatalog { CpPack *packs[CP_PACKS]; unsigned count; } CpCatalog;

/* Strict, bounded UTF-8 line format; see docs/content-packs.md. Transactional:
 * malformed/duplicate input never changes an existing catalog or output. */
int cp_manifest_read(const char *path, CpPack *out, char *error, size_t cap);
int cp_catalog_add(CpCatalog *catalog, const CpPack *pack, char *error, size_t cap);
const CpPack *cp_catalog_find(const CpCatalog *catalog, const char *id);
void cp_catalog_free(CpCatalog *catalog);
int cp_id_valid(const char *id);
int cp_hash_parse(const char *hex, uint8_t hash[32]);
void cp_hash_format(const uint8_t hash[32], char hex[65]);
/* Resolve pack/cup by stable ID; a missing selection fails, never aliases. */
const CpCup *cp_catalog_cup(const CpCatalog *catalog, const char *key,
                            const CpPack **pack);

/* Apply to a newly allocated image, preserving source and outputs on failure.
 * IPS and BPS are sniffed by magic, not extension. BPS verifies all CRCs.
 * cp_pack_apply additionally verifies BOTH manifest SHA-256 identities. */
int cp_patch_apply(const uint8_t *source, size_t source_size,
                   const uint8_t *patch, size_t patch_size,
                   uint8_t **target, size_t *target_size, char *error, size_t cap);
int cp_pack_apply(const CpPack *pack, const uint8_t *source, size_t source_size,
                  const char *patch_path, uint8_t **target, size_t *target_size,
                  char *error, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
