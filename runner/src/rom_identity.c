/*
 * rom_identity.c - name a SNES ROM image and explain why it is refused.
 * See rom_identity.h.
 */
#include "rom_identity.h"

#include "crc32.h"
#include "sha1.h"
#include "sha256.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void appendf(char *out, size_t cap, const char *fmt, ...) {
  size_t len = strlen(out);
  if (len + 1 >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(out + len, cap - len, fmt, ap);
  va_end(ap);
}

/* Full-length, case-insensitive comparison; an empty `want` never matches. */
static int hex_equal(const char *have, const char *want) {
  if (!have || !want || !want[0]) return 0;
  size_t i = 0;
  for (; have[i] && want[i]; ++i)
    if (tolower((unsigned char)have[i]) != tolower((unsigned char)want[i]))
      return 0;
  return have[i] == '\0' && want[i] == '\0';
}

static void to_hex(const uint8_t *d, size_t n, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[i * 2] = digits[d[i] >> 4];
    out[i * 2 + 1] = digits[d[i] & 15];
  }
  out[n * 2] = '\0';
}

int snes_rom_revision_matches(const SnesRomRevision *r, const char *sha256_hex,
                              const char *sha1_hex, const char *crc32_hex) {
  int given = 0;
  if (!r) return 0;
  if (r->sha256_hex && r->sha256_hex[0]) {
    ++given;
    if (!hex_equal(sha256_hex, r->sha256_hex)) return 0;
  }
  if (r->sha1_hex && r->sha1_hex[0]) {
    ++given;
    if (!hex_equal(sha1_hex, r->sha1_hex)) return 0;
  }
  if (r->crc32_hex && r->crc32_hex[0]) {
    ++given;
    if (!hex_equal(crc32_hex, r->crc32_hex)) return 0;
  }
  return given > 0;
}

int snes_rom_revision_find_sha256(const SnesRomRevision *catalog, int n,
                                  const char *sha256_hex) {
  for (int i = 0; catalog && i < n; ++i)
    if (hex_equal(sha256_hex, catalog[i].sha256_hex)) return i;
  return -1;
}

static const char *archive_kind(const uint8_t *d, size_t n) {
  if (n >= 4 && d[0] == 'P' && d[1] == 'K' &&
      ((d[2] == 3 && d[3] == 4) || (d[2] == 5 && d[3] == 6) ||
       (d[2] == 7 && d[3] == 8)))
    return "ZIP";
  if (n >= 6 && memcmp(d, "7z\xBC\xAF\x27\x1C", 6) == 0) return "7-Zip";
  if (n >= 6 && memcmp(d, "Rar!\x1A\x07", 6) == 0) return "RAR";
  if (n >= 2 && d[0] == 0x1F && d[1] == 0x8B) return "gzip";
  return NULL;
}

/* ---- internal SNES header ------------------------------------------------ */

static const char *const kRegions[] = {
  "Japan", "North America", "Europe", "Sweden", "Finland", "Denmark",
  "France", "Netherlands", "Spain", "Germany", "Italy", "China", "Indonesia",
  "South Korea", "International", "Canada", "Brazil", "Australia",
};

/* How plausible a header at `base` (the title's offset) is. LoROM keeps it
 * at 0x7FC0, HiROM at 0xFFC0, the extended maps 4 MiB further on. */
static int header_score(const uint8_t *img, size_t len, size_t base) {
  if (base + 0x40 > len) return -1;
  const uint8_t *h = img + base;
  int score = 0;
  const unsigned complement = h[0x1C] | (h[0x1D] << 8);
  const unsigned checksum = h[0x1E] | (h[0x1F] << 8);
  if ((complement ^ checksum) == 0xFFFF) score += 4;
  const unsigned map = h[0x15];
  const int hi_slot = (base & 0xFFFF) == 0xFFC0;
  const unsigned kind = map & 0x0F;
  if ((map & 0xE0) == 0x20 &&
      (hi_slot ? (kind == 1 || kind == 5) : (kind == 0 || kind == 2 || kind == 3)))
    score += 2;
  int printable = 0;
  for (int i = 0; i < 21; ++i)
    if ((h[i] >= 0x20 && h[i] < 0x7F) || (h[i] >= 0xA0 && h[i] <= 0xDF)) ++printable;
  if (printable >= 19) score += 2;
  if ((h[0x3C] | (h[0x3D] << 8)) >= 0x8000) score += 1;
  return score;
}

/* The SNES header checksum: a 16-bit byte sum, with a ROM that is not a power
 * of two mirrored up to the next one as the hardware sees it. */
static unsigned snes_checksum(const uint8_t *d, size_t n) {
  size_t base = 1;
  while (base * 2 <= n) base *= 2;
  uint32_t sum = 0;
  for (size_t i = 0; i < base; ++i) sum += d[i];
  if (base < n) {
    const size_t rest = n - base;
    for (size_t i = 0; i < base; ++i) sum += d[base + i % rest];
  }
  return sum & 0xFFFF;
}

static int describe_header(const uint8_t *img, size_t len, int compact,
                           char *out, size_t cap) {
  static const size_t kBases[] = {0x7FC0, 0xFFC0, 0x40FFC0, 0x407FC0};
  if (cap) out[0] = '\0';
  if (!img || cap == 0) return 0;
  size_t best = 0;
  int best_score = -1;
  for (size_t i = 0; i < sizeof(kBases) / sizeof(kBases[0]); ++i) {
    int s = header_score(img, len, kBases[i]);
    if (s > best_score) { best_score = s; best = kBases[i]; }
  }
  if (best_score < 6) return 0;
  const uint8_t *h = img + best;
  char title[22];
  int t = 0;
  for (int i = 0; i < 21; ++i)
    title[t++] = (h[i] >= 0x20 && h[i] < 0x7F && h[i] != '"') ? (char)h[i] : ' ';
  while (t > 0 && title[t - 1] == ' ') --t;
  title[t] = '\0';
  const unsigned region = h[0x19];
  const char *region_name =
      region < sizeof(kRegions) / sizeof(kRegions[0]) ? kRegions[region] : NULL;
  const unsigned stored = h[0x1E] | (h[0x1F] << 8);
  const unsigned complement = h[0x1C] | (h[0x1D] << 8);
  const int consistent = (stored ^ complement) == 0xFFFF;
  const int sum_ok = consistent && snes_checksum(img, len) == stored;
  if (compact) {
    snprintf(out, cap, "header \"%s\" v1.%u", title, h[0x1B]);
    if (region_name) appendf(out, cap, ", %s", region_name);
    if (!sum_ok) appendf(out, cap, ", checksum mismatch");
  } else {
    snprintf(out, cap, "Its internal header reads \"%s\", version 1.%u", title,
             h[0x1B]);
    if (region_name) appendf(out, cap, ", %s", region_name);
    appendf(out, cap, ".");
    if (!sum_ok)
      appendf(out, cap,
              " Its data no longer matches the header checksum, so it has "
              "been patched (hack, translation, randomizer, MSU-1 patch) or "
              "is a bad dump.");
  }
  return 1;
}

int snes_rom_describe_header(const uint8_t *image, size_t len, char *out,
                             size_t cap) {
  return describe_header(image, len, 0, out, cap);
}

/* ---- verdicts -------------------------------------------------------------- */

static void requirement(const SnesRomRevision *catalog, int n,
                        const char *required_sha256_hex, char *out, size_t cap) {
  const int want = snes_rom_revision_find_sha256(catalog, n, required_sha256_hex);
  snprintf(out, cap, "%s (SHA-256 %s)",
           want >= 0 && catalog[want].label && catalog[want].label[0]
               ? catalog[want].label : "the original ROM",
           required_sha256_hex);
}

SnesRomVerdict snes_rom_identity_check_image(
    const uint8_t *raw, size_t raw_len, const SnesRomRevision *catalog, int n,
    const char *required_sha256_hex, const char *subject, const char *help,
    SnesRomFacts *facts, char *explain, size_t explain_cap) {
  SnesRomFacts local;
  SnesRomFacts *f = facts ? facts : &local;
  memset(f, 0, sizeof(*f));
  f->revision = -1;
  if (explain && explain_cap) explain[0] = '\0';
  const int compact = subject == NULL;

  if (!raw || raw_len == 0) {
    f->verdict = kSnesRom_Unreadable;
    if (explain)
      snprintf(explain, explain_cap, "%s",
               compact ? "Cannot read this file (it is empty)."
                       : "This file is empty.");
    return f->verdict;
  }
  f->archive_kind = archive_kind(raw, raw_len);
  if (f->archive_kind) {
    f->verdict = kSnesRom_Archive;
    if (explain)
      snprintf(explain, explain_cap,
               compact ? "This is a %s archive: extract the ROM from it first."
                       : "This is a %s archive, not a ROM. Extract it first, "
                         "then select the .sfc or .smc file that was inside.",
               f->archive_kind);
    return f->verdict;
  }

  const size_t header = (raw_len % 1024 == 512) ? 512 : 0;
  const uint8_t *img = raw + header;
  const size_t len = raw_len - header;
  uint8_t sha256[32], sha1[20];
  sha256_compute(img, len, sha256);
  snes_sha1_compute(img, len, sha1);
  to_hex(sha256, 32, f->sha256_hex);
  to_hex(sha1, 20, f->sha1_hex);
  snprintf(f->crc32_hex, sizeof(f->crc32_hex), "%08X",
           (unsigned)crc32_compute(img, len));

  /* A catalog entry that is the required dump outranks any other match. */
  for (int i = 0; catalog && i < n; ++i) {
    if (!snes_rom_revision_matches(&catalog[i], f->sha256_hex, f->sha1_hex,
                                   f->crc32_hex))
      continue;
    if (f->revision < 0 || hex_equal(catalog[i].sha256_hex, required_sha256_hex))
      f->revision = i;
  }
  if (!required_sha256_hex || !required_sha256_hex[0] ||
      hex_equal(f->sha256_hex, required_sha256_hex)) {
    f->verdict = kSnesRom_Ok;
    return f->verdict;
  }
  if (!explain) {
    f->verdict = f->revision >= 0 ? kSnesRom_Wrong : kSnesRom_Unknown;
    return f->verdict;
  }

  char need[256];
  requirement(catalog, n, required_sha256_hex, need, sizeof(need));
  if (f->revision >= 0) {
    const SnesRomRevision *r = &catalog[f->revision];
    const char *label = r->label && r->label[0] ? r->label : "a different ROM";
    f->verdict = kSnesRom_Wrong;
    if (compact) {
      snprintf(explain, explain_cap, "Wrong ROM: this is %s. Select %s.", label,
               need);
    } else {
      snprintf(explain, explain_cap, "This is %s. %s needs %s.", label, subject,
               need);
      if (r->note && r->note[0]) appendf(explain, explain_cap, " %s", r->note);
    }
  } else {
    char seen[320];
    f->verdict = kSnesRom_Unknown;
    const int described = describe_header(img, len, compact, seen, sizeof(seen));
    if (compact) {
      snprintf(explain, explain_cap, "Not a known dump");
      if (described) appendf(explain, explain_cap, " (%s)", seen);
      appendf(explain, explain_cap, ". Select %s.", need);
    } else {
      if (described) snprintf(explain, explain_cap, "%s ", seen);
      appendf(explain, explain_cap,
              "This file does not match any known dump (its SHA-256 is %s). "
              "It may be another revision or region, a hack, a translation or "
              "a bad dump. %s needs %s.",
              f->sha256_hex, subject, need);
    }
  }
  if (!compact && help && help[0]) appendf(explain, explain_cap, " %s", help);
  return f->verdict;
}

SnesRomVerdict snes_rom_identity_check_file(
    const char *path, const SnesRomRevision *catalog, int n,
    const char *required_sha256_hex, const char *subject, const char *help,
    SnesRomFacts *facts, char *explain, size_t explain_cap) {
  uint8_t *data = NULL;
  long size = -1;
  FILE *f = path && path[0] ? fopen(path, "rb") : NULL;
  if (f) {
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size > 0 && fseek(f, 0, SEEK_SET) == 0) {
      data = (uint8_t *)malloc((size_t)size);
      if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
      }
    }
    fclose(f);
  }
  if (!data && size != 0) {
    SnesRomFacts local;
    SnesRomFacts *out = facts ? facts : &local;
    memset(out, 0, sizeof(*out));
    out->revision = -1;
    out->verdict = kSnesRom_Unreadable;
    if (explain && explain_cap)
      snprintf(explain, explain_cap, "%s",
               subject ? "This file could not be read. It may have been moved "
                         "or deleted."
                       : "Cannot read this file.");
    return out->verdict;
  }
  SnesRomVerdict v = snes_rom_identity_check_image(
      data, data ? (size_t)size : 0, catalog, n, required_sha256_hex, subject,
      help, facts, explain, explain_cap);
  free(data);
  return v;
}
