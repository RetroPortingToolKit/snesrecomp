#include "content_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t crc32(const uint8_t *p, size_t size) {
    uint32_t crc = ~0u;
    while (size--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
static int number(const uint8_t *patch, size_t end, size_t *at, uint64_t *out) {
    uint64_t value = 0, shift = 1;
    for (unsigned i = 0; i < 8 && *at < end; ++i) {
        uint8_t b = patch[(*at)++]; value += (b & 127u) * shift;
        if (b & 128) { *out = value; return 1; }
        shift <<= 7; value += shift;
    }
    return 0;
}
static uint8_t *bps(const uint8_t *s, size_t sn, const uint8_t *p, size_t pn, size_t *size) {
    if (pn < 16 || crc32(p, pn-4) != le32(p+pn-4) || crc32(s, sn) != le32(p+pn-12)) return NULL;
    size_t at = 4, end = pn-12, written = 0;
    uint64_t source_size, target_size, metadata;
    if (!number(p, end, &at, &source_size) || source_size != sn ||
        !number(p, end, &at, &target_size) || !target_size || target_size > CP_ROM_LIMIT ||
        !number(p, end, &at, &metadata) || metadata > end-at) return NULL;
    at += (size_t)metadata;
    uint8_t *out = malloc((size_t)target_size);
    if (!out) return NULL;
    int64_t source_relative = 0, target_relative = 0;
    while (written < target_size) {
        uint64_t action, encoded;
        if (!number(p, end, &at, &action)) goto bad;
        size_t n = (size_t)(action >> 2) + 1;
        if (n > target_size-written) goto bad;
        switch (action & 3) {
        case 0:
            if (written > sn || n > sn-written) goto bad;
            memcpy(out+written, s+written, n); break;
        case 1:
            if (n > end-at) goto bad;
            memcpy(out+written, p+at, n); at += n; break;
        case 2:
            if (!number(p, end, &at, &encoded)) goto bad;
            source_relative += encoded & 1 ? -(int64_t)(encoded >> 1) : (int64_t)(encoded >> 1);
            if (source_relative < 0 || (uint64_t)source_relative > sn || n > sn-(size_t)source_relative) goto bad;
            memcpy(out+written, s+(size_t)source_relative, n); source_relative += (int64_t)n; break;
        case 3:
            if (!number(p, end, &at, &encoded)) goto bad;
            target_relative += encoded & 1 ? -(int64_t)(encoded >> 1) : (int64_t)(encoded >> 1);
            if (target_relative < 0 || (uint64_t)target_relative >= written) goto bad;
            /* Overlap intentionally repeats bytes emitted by this command. */
            for (size_t i = 0; i < n; ++i) out[written+i] = out[(size_t)target_relative++];
            break;
        }
        written += n;
    }
    if (at != end || crc32(out, written) != le32(p+pn-8)) goto bad;
    *size = written; return out;
bad:
    free(out); return NULL;
}
static size_t be(const uint8_t *p, unsigned n) {
    size_t v = 0; while (n--) v = (v << 8) | *p++; return v;
}
static uint8_t *ips(const uint8_t *s, size_t sn, const uint8_t *p, size_t pn, size_t *size) {
    /* A fixed upper bound avoids arithmetic/reallocation surprises from IPS,
     * whose records may overlap and need not be ordered. */
    uint8_t *out = calloc(1, CP_ROM_LIMIT);
    if (!out) return NULL;
    memcpy(out, s, sn);
    size_t at = 5, length = sn;
    for (;;) {
        if (pn-at < 3) goto bad;
        if (!memcmp(p+at, "EOF", 3)) { at += 3; break; }
        if (pn-at < 5) goto bad;
        size_t offset = be(p+at, 3), n = be(p+at+3, 2); at += 5;
        int run = n == 0;
        if (run) {
            if (pn-at < 3) goto bad;
            n = be(p+at, 2); at += 2;
            if (!n) goto bad;
        }
        if (n > CP_ROM_LIMIT-offset || pn-at < (run ? 1 : n)) goto bad;
        if (run) memset(out+offset, p[at++], n);
        else { memcpy(out+offset, p+at, n); at += n; }
        if (length < offset+n) length = offset+n;
    }
    if (pn-at == 3) length = be(p+at, 3);
    else if (at != pn) goto bad;
    if (!length || length > CP_ROM_LIMIT) goto bad;
    *size = length; return out;
bad:
    free(out); return NULL;
}
int cp_patch_apply(const uint8_t *source, size_t source_size,
                   const uint8_t *patch, size_t patch_size,
                   uint8_t **target, size_t *target_size, char *error, size_t cap) {
    uint8_t *out = NULL; size_t size = 0;
    if (source && source_size && source_size <= CP_ROM_LIMIT && patch && patch_size <= CP_ROM_LIMIT*2) {
        if (patch_size >= 8 && !memcmp(patch, "PATCH", 5)) out = ips(source, source_size, patch, patch_size, &size);
        else if (patch_size >= 16 && !memcmp(patch, "BPS1", 4)) out = bps(source, source_size, patch, patch_size, &size);
    }
    if (!out) {
        if (error && cap) snprintf(error, cap, "Invalid IPS/BPS patch, CRC, size or copy range");
        return 0;
    }
    *target = out; *target_size = size; return 1;
}
