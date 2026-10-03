#include "rom_patch.h"

#include <stdlib.h>
#include <string.h>

#include "crc32.h"

static uint32_t be24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

RomPatchFormat snes_rom_patch_detect(const uint8_t *patch, size_t patch_size) {
    if (!patch) return kRomPatchFormat_Unknown;
    if (patch_size >= 8 && memcmp(patch, "PATCH", 5) == 0) return kRomPatchFormat_Ips;
    if (patch_size >= 16 && memcmp(patch, "BPS1", 4) == 0) return kRomPatchFormat_Bps;
    return kRomPatchFormat_Unknown;
}

const char *snes_rom_patch_status_text(RomPatchStatus status) {
    switch (status) {
    case kRomPatch_Ok: return "ok";
    case kRomPatch_Invalid: return "patch is not a valid IPS or BPS file";
    case kRomPatch_SourceMismatch: return "patch was made for a different source ROM";
    case kRomPatch_TargetMismatch: return "patched result failed its own checksum";
    case kRomPatch_TooLarge: return "patched result exceeds the size limit";
    case kRomPatch_NoMemory: return "out of memory";
    }
    return "unknown patch error";
}

/* Grow-to-fit buffer used by both appliers. */
typedef struct Image {
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t max_size;
} Image;

static int image_reserve(Image *img, size_t needed) {
    if (needed > img->max_size) return 0;
    if (needed <= img->capacity) return 1;
    size_t cap = img->capacity ? img->capacity : 0x8000;
    while (cap < needed) cap *= 2;
    if (cap > img->max_size) cap = img->max_size;
    uint8_t *grown = realloc(img->data, cap);
    if (!grown) return 0;
    /* Bytes between the old and new logical end must read as zero, exactly
     * like a file extended by a write past EOF. */
    memset(grown + img->capacity, 0, cap - img->capacity);
    img->data = grown;
    img->capacity = cap;
    return 1;
}

static RomPatchStatus apply_ips(const uint8_t *source, size_t source_size,
                                const uint8_t *patch, size_t patch_size,
                                Image *img) {
    if (!image_reserve(img, source_size ? source_size : 1))
        return source_size > img->max_size ? kRomPatch_TooLarge : kRomPatch_NoMemory;
    memcpy(img->data, source, source_size);
    img->size = source_size;

    size_t at = 5;
    for (;;) {
        if (patch_size - at < 3) return kRomPatch_Invalid;
        if (memcmp(patch + at, "EOF", 3) == 0) {
            at += 3;
            break;
        }
        uint32_t offset = be24(patch + at);
        at += 3;
        if (patch_size - at < 2) return kRomPatch_Invalid;
        uint32_t size = be16(patch + at);
        at += 2;
        if (size == 0) {
            if (patch_size - at < 3) return kRomPatch_Invalid;
            uint32_t rle = be16(patch + at);
            uint8_t value = patch[at + 2];
            at += 3;
            if (rle == 0) return kRomPatch_Invalid;
            size_t end = (size_t)offset + rle;
            if (end > img->max_size) return kRomPatch_TooLarge;
            if (!image_reserve(img, end)) return kRomPatch_NoMemory;
            memset(img->data + offset, value, rle);
            if (end > img->size) img->size = end;
        } else {
            if (patch_size - at < size) return kRomPatch_Invalid;
            size_t end = (size_t)offset + size;
            if (end > img->max_size) return kRomPatch_TooLarge;
            if (!image_reserve(img, end)) return kRomPatch_NoMemory;
            memcpy(img->data + offset, patch + at, size);
            at += size;
            if (end > img->size) img->size = end;
        }
    }
    /* Optional truncation extension: a 3-byte target length after EOF. */
    if (patch_size - at == 3) {
        uint32_t truncate = be24(patch + at);
        if (truncate < img->size) img->size = truncate;
        at += 3;
    }
    if (at != patch_size) return kRomPatch_Invalid;
    return kRomPatch_Ok;
}

static int bps_varint(const uint8_t *patch, size_t limit, size_t *at,
                      uint64_t *out) {
    uint64_t value = 0, shift = 1;
    for (;;) {
        if (*at >= limit) return 0;
        uint8_t byte = patch[(*at)++];
        value += (uint64_t)(byte & 0x7f) * shift;
        if (byte & 0x80) break;
        shift <<= 7;
        value += shift;
        if (shift > ((uint64_t)1 << 56)) return 0;
    }
    *out = value;
    return 1;
}

static RomPatchStatus apply_bps(const uint8_t *source, size_t source_size,
                                const uint8_t *patch, size_t patch_size,
                                Image *img) {
    if (patch_size < 4 + 12) return kRomPatch_Invalid;
    const size_t footer = patch_size - 12;
    uint32_t source_crc = le32(patch + footer);
    uint32_t target_crc = le32(patch + footer + 4);
    uint32_t patch_crc = le32(patch + footer + 8);
    if (crc32_compute(patch, patch_size - 4) != patch_crc) return kRomPatch_Invalid;

    size_t at = 4;
    uint64_t src_size = 0, tgt_size = 0, meta_size = 0;
    if (!bps_varint(patch, footer, &at, &src_size) ||
        !bps_varint(patch, footer, &at, &tgt_size) ||
        !bps_varint(patch, footer, &at, &meta_size))
        return kRomPatch_Invalid;
    if (meta_size > footer - at) return kRomPatch_Invalid;
    at += (size_t)meta_size;
    if (src_size != source_size) return kRomPatch_SourceMismatch;
    if (crc32_compute(source, source_size) != source_crc) return kRomPatch_SourceMismatch;
    if (tgt_size > img->max_size) return kRomPatch_TooLarge;
    if (!image_reserve(img, tgt_size ? (size_t)tgt_size : 1)) return kRomPatch_NoMemory;
    img->size = (size_t)tgt_size;

    size_t out = 0;
    size_t source_rel = 0, target_rel = 0;
    while (at < footer) {
        uint64_t word = 0;
        if (!bps_varint(patch, footer, &at, &word)) return kRomPatch_Invalid;
        uint32_t action = (uint32_t)(word & 3);
        uint64_t length = (word >> 2) + 1;
        if (length > img->size - out) return kRomPatch_Invalid;
        switch (action) {
        case 0: /* SourceRead */
            if (out + length > source_size) return kRomPatch_Invalid;
            memcpy(img->data + out, source + out, (size_t)length);
            out += (size_t)length;
            break;
        case 1: /* TargetRead */
            if (length > footer - at) return kRomPatch_Invalid;
            memcpy(img->data + out, patch + at, (size_t)length);
            at += (size_t)length;
            out += (size_t)length;
            break;
        case 2: { /* SourceCopy */
            uint64_t raw = 0;
            if (!bps_varint(patch, footer, &at, &raw)) return kRomPatch_Invalid;
            int64_t delta = (raw & 1) ? -(int64_t)(raw >> 1) : (int64_t)(raw >> 1);
            if (delta < 0 && (uint64_t)(-delta) > source_rel) return kRomPatch_Invalid;
            source_rel = (size_t)((int64_t)source_rel + delta);
            if (source_rel + length > source_size) return kRomPatch_Invalid;
            memcpy(img->data + out, source + source_rel, (size_t)length);
            source_rel += (size_t)length;
            out += (size_t)length;
            break;
        }
        case 3: { /* TargetCopy: byte-wise, may overlap forward (RLE idiom) */
            uint64_t raw = 0;
            if (!bps_varint(patch, footer, &at, &raw)) return kRomPatch_Invalid;
            int64_t delta = (raw & 1) ? -(int64_t)(raw >> 1) : (int64_t)(raw >> 1);
            if (delta < 0 && (uint64_t)(-delta) > target_rel) return kRomPatch_Invalid;
            target_rel = (size_t)((int64_t)target_rel + delta);
            if (target_rel >= out) return kRomPatch_Invalid;
            for (uint64_t i = 0; i < length; i++)
                img->data[out++] = img->data[target_rel++];
            break;
        }
        }
    }
    if (out != img->size) return kRomPatch_Invalid;
    if (crc32_compute(img->data, img->size) != target_crc) return kRomPatch_TargetMismatch;
    return kRomPatch_Ok;
}

RomPatchStatus snes_rom_patch_apply(const uint8_t *source, size_t source_size,
                               const uint8_t *patch, size_t patch_size,
                               size_t max_size,
                               uint8_t **out, size_t *out_size) {
    if (out) *out = NULL;
    if (out_size) *out_size = 0;
    if (!source || !patch || !out || !out_size || max_size == 0)
        return kRomPatch_Invalid;
    Image img = {0};
    img.max_size = max_size;
    RomPatchStatus status;
    switch (snes_rom_patch_detect(patch, patch_size)) {
    case kRomPatchFormat_Ips:
        status = apply_ips(source, source_size, patch, patch_size, &img);
        break;
    case kRomPatchFormat_Bps:
        status = apply_bps(source, source_size, patch, patch_size, &img);
        break;
    default:
        status = kRomPatch_Invalid;
        break;
    }
    if (status != kRomPatch_Ok) {
        free(img.data);
        return status;
    }
    *out = img.data;
    *out_size = img.size;
    return kRomPatch_Ok;
}
