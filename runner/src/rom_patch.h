#ifndef SNESRECOMP_ROM_PATCH_H
#define SNESRECOMP_ROM_PATCH_H

/*
 * In-memory ROM patching for content variants: IPS and BPS.
 *
 * A variant that is a ROM hack declares `[[patch]]` in its package manifest;
 * at activation the runtime applies that patch to the verified source image
 * IN MEMORY, verifies the result against the manifest's target digest, and
 * only then lets a recompiled module run against it. Nothing patched is ever
 * written to disk, and a patch whose result does not hash as declared never
 * executes -- the same all-or-nothing rule F-Zero's BS Deluxe importer
 * established, now available to every title through the manifest.
 *
 * Both formats are applied by the book:
 *   IPS  -- "PATCH", records {offset:3, size:2, data} with size 0 meaning RLE
 *           {rle_size:2, byte}, "EOF", optional 3-byte truncation length.
 *   BPS  -- "BPS1", varint source/target/metadata sizes, actions
 *           (SourceRead / TargetRead / SourceCopy / TargetCopy), then CRC32 of
 *           source, target and patch; every CRC is checked.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RomPatchStatus {
    kRomPatch_Ok = 0,
    kRomPatch_Invalid,        /* not a recognizable IPS/BPS, or malformed */
    kRomPatch_SourceMismatch, /* BPS: source size/CRC does not match */
    kRomPatch_TargetMismatch, /* BPS: produced bytes do not match target CRC */
    kRomPatch_TooLarge,       /* result would exceed max_size */
    kRomPatch_NoMemory,
} RomPatchStatus;

typedef enum RomPatchFormat {
    kRomPatchFormat_Unknown = 0,
    kRomPatchFormat_Ips,
    kRomPatchFormat_Bps,
} RomPatchFormat;

RomPatchFormat rom_patch_detect(const uint8_t *patch, size_t patch_size);

/* Apply `patch` to `source`, producing a malloc-owned image in (*out, *out_size).
 * `max_size` bounds the result (an IPS may grow the image; 16 MiB is a sane
 * cap for SNES). On any failure nothing is allocated and *out is NULL. */
RomPatchStatus rom_patch_apply(const uint8_t *source, size_t source_size,
                               const uint8_t *patch, size_t patch_size,
                               size_t max_size,
                               uint8_t **out, size_t *out_size);

const char *rom_patch_status_text(RomPatchStatus status);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_ROM_PATCH_H */
