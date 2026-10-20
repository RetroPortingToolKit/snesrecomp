/* A refused ROM is named, not just reported as a digest mismatch. Builds
 * synthetic LoROM images with valid internal headers, so it stays ROM-free. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rom_identity.h"
#include "sha256.h"

static void fail(const char *message, const char *detail) {
    fprintf(stderr, "rom_identity_test: FAIL: %s\n  %s\n", message,
            detail ? detail : "");
    exit(1);
}

static void require_has(const char *text, const char *part, const char *label) {
    if (!strstr(text, part)) {
        char detail[2048];
        snprintf(detail, sizeof(detail), "missing \"%s\" in \"%s\"", part, text);
        fail(label, detail);
    }
}

static void to_hex(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; ++i) snprintf(out + i * 2, 3, "%02x", d[i]);
}

/* 64 KiB LoROM image with an internal header naming `title`, `version` and
 * `region`, and a correct checksum/complement pair. */
static void make_rom(uint8_t *rom, size_t len, const char *title,
                     uint8_t version, uint8_t region, uint8_t fill) {
    memset(rom, fill, len);
    uint8_t *h = rom + 0x7FC0;
    memset(h, ' ', 21);
    memcpy(h, title, strlen(title));
    h[0x15] = 0x20;  /* LoROM */
    h[0x19] = region;
    h[0x1B] = version;
    h[0x3C] = 0x00; h[0x3D] = 0x80;  /* reset vector $8000 */
    h[0x1C] = 0xFF; h[0x1D] = 0xFF; h[0x1E] = 0x00; h[0x1F] = 0x00;
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum += rom[i];
    const unsigned checksum = sum & 0xFFFF;
    h[0x1E] = (uint8_t)checksum; h[0x1F] = (uint8_t)(checksum >> 8);
    h[0x1C] = (uint8_t)~checksum; h[0x1D] = (uint8_t)(~checksum >> 8);
}

static void write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n) fail("could not write fixture", path);
    fclose(f);
}

enum { kLen = 0x10000 };

int main(void) {
    static uint8_t good[kLen], old[kLen], other[kLen], hacked[kLen];
    static uint8_t headered[kLen + 512];
    make_rom(good, kLen, "TEST QUEST", 1, 1, 0x11);
    make_rom(old, kLen, "TEST QUEST", 0, 1, 0x22);
    make_rom(other, kLen, "TEST QUEST 3", 0, 1, 0x33);
    make_rom(hacked, kLen, "TEST QUEST", 0, 1, 0x44);
    hacked[0x100] ^= 0x5A;  /* patched after the checksum was written */
    memset(headered, 0, 512);
    memcpy(headered + 512, good, kLen);

    uint8_t d[32];
    char good_sha[65], old_sha[65], other_sha[65], hacked_sha[65];
    sha256_compute(good, kLen, d);   to_hex(d, 32, good_sha);
    sha256_compute(old, kLen, d);    to_hex(d, 32, old_sha);
    sha256_compute(other, kLen, d);  to_hex(d, 32, other_sha);
    sha256_compute(hacked, kLen, d); to_hex(d, 32, hacked_sha);

    const SnesRomRevision catalog[] = {
        {"Test Quest (USA) (Rev 1)", good_sha, NULL, NULL, NULL},
        {"Test Quest (USA)", old_sha, NULL, NULL, "Rev 0 softlocks in stage 2."},
        {"Test Quest 3 (USA)", other_sha, NULL, NULL, NULL},
    };
    char why[1536];
    SnesRomFacts facts;

    /* The required dump, with or without a copier header. */
    if (snes_rom_identity_check_image(good, kLen, catalog, 3, good_sha, "Test Quest",
                                      NULL, &facts, why, sizeof(why)) != kSnesRom_Ok)
        fail("required dump refused", why);
    if (facts.revision != 0) fail("required dump not identified", NULL);
    if (snes_rom_identity_check_image(headered, sizeof(headered), catalog, 3,
                                      good_sha, "Test Quest", NULL, &facts, why,
                                      sizeof(why)) != kSnesRom_Ok)
        fail("headered dump refused", why);
    if (strcmp(facts.sha256_hex, good_sha) != 0) fail("copier header hashed", NULL);

    /* Other revision: named, requirement with SHA-256, note, help. */
    if (snes_rom_identity_check_image(old, kLen, catalog, 3, good_sha, "Test Quest",
                                      "Headers are fine.", &facts, why,
                                      sizeof(why)) != kSnesRom_Wrong)
        fail("old revision not reported as wrong", why);
    require_has(why, "This is Test Quest (USA).", "names the revision");
    require_has(why, "Test Quest needs Test Quest (USA) (Rev 1) (SHA-256 ", "requirement");
    require_has(why, good_sha, "required digest");
    require_has(why, "Rev 0 softlocks in stage 2.", "note");
    require_has(why, "Headers are fine.", "help");

    /* Compact form for a status line. */
    snes_rom_identity_check_image(other, kLen, catalog, 3, good_sha, NULL, "unused",
                                  &facts, why, 256);
    require_has(why, "Wrong ROM: this is Test Quest 3 (USA). Select Test Quest (USA) (Rev 1)",
                "compact wrong form");
    if (strstr(why, "unused")) fail("compact form carries help", why);

    /* Unknown dump: internal header and checksum verdict, its own digest. */
    if (snes_rom_identity_check_image(hacked, kLen, catalog, 3, good_sha,
                                      "Test Quest", NULL, &facts, why,
                                      sizeof(why)) != kSnesRom_Unknown)
        fail("hacked dump not unknown", why);
    require_has(why, "Its internal header reads \"TEST QUEST\", version 1.0, North America.",
                "header description");
    require_has(why, "no longer matches the header checksum", "checksum mismatch");
    require_has(why, hacked_sha, "file digest");
    require_has(why, good_sha, "required digest");
    snes_rom_identity_check_image(hacked, kLen, catalog, 3, good_sha, NULL, NULL,
                                  &facts, why, 256);
    require_has(why, "Not a known dump (header \"TEST QUEST\" v1.0, North America, "
                     "checksum mismatch). Select Test Quest (USA) (Rev 1)",
                "compact unknown form");
    if (strlen(why) >= 255) fail("compact form truncated", why);

    /* An untouched but uncatalogued dump keeps a valid checksum. */
    {
        static uint8_t clean[kLen];
        make_rom(clean, kLen, "TEST QUEST", 2, 0, 0x55);
        snes_rom_identity_check_image(clean, kLen, catalog, 3, good_sha, "Test Quest",
                                      NULL, &facts, why, sizeof(why));
        require_has(why, "\"TEST QUEST\", version 1.2, Japan.", "clean header");
        if (strstr(why, "checksum")) fail("clean dump blamed on checksum", why);
    }

    /* Without a catalog the requirement still gives the digest. */
    snes_rom_identity_check_image(old, kLen, NULL, 0, good_sha, "Test Quest", NULL,
                                  &facts, why, sizeof(why));
    require_has(why, "Test Quest needs the original ROM (SHA-256 ", "no-catalog requirement");

    /* Archives and unreadable files. */
    {
        const uint8_t zip[] = {'P', 'K', 3, 4, 20, 0, 0, 0};
        if (snes_rom_identity_check_image(zip, sizeof(zip), catalog, 3, good_sha,
                                          "Test Quest", NULL, &facts, why,
                                          sizeof(why)) != kSnesRom_Archive)
            fail("zip not an archive", why);
        require_has(why, "ZIP archive", "archive kind");
        const uint8_t sevenz[] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C, 0, 4};
        snes_rom_identity_check_image(sevenz, sizeof(sevenz), catalog, 3, good_sha,
                                      "Test Quest", NULL, &facts, why, sizeof(why));
        require_has(why, "7-Zip archive", "7z kind");
    }
    if (snes_rom_identity_check_file("rom_identity_missing.sfc", catalog, 3, good_sha,
                                     "Test Quest", NULL, &facts, why,
                                     sizeof(why)) != kSnesRom_Unreadable)
        fail("missing file not unreadable", why);

    /* File path agrees with the image path. */
    write_file("rom_identity_old.smc", headered, sizeof(headered));
    if (snes_rom_identity_check_file("rom_identity_old.smc", catalog, 3, good_sha,
                                     "Test Quest", NULL, &facts, why,
                                     sizeof(why)) != kSnesRom_Ok)
        fail("file check refused the headered required dump", why);
    remove("rom_identity_old.smc");

    /* No requirement: anything readable is accepted. */
    if (snes_rom_identity_check_image(old, kLen, catalog, 3, NULL, "Test Quest",
                                      NULL, &facts, why, sizeof(why)) != kSnesRom_Ok)
        fail("no requirement refused a ROM", why);
    if (facts.revision != 1) fail("no requirement: revision not identified", NULL);

    /* Matching rules: case-insensitive, and every given digest must agree. */
    {
        char upper[65];
        for (int i = 0; i < 65; ++i)
            upper[i] = (good_sha[i] >= 'a' && good_sha[i] <= 'f')
                           ? (char)(good_sha[i] - 32) : good_sha[i];
        SnesRomRevision r = {"x", upper, NULL, NULL, NULL};
        if (!snes_rom_revision_matches(&r, good_sha, "", "")) fail("case-insensitive", NULL);
        r.crc32_hex = "00000000";
        if (snes_rom_revision_matches(&r, good_sha, "", "12345678"))
            fail("disagreeing CRC still matched", NULL);
        SnesRomRevision none = {"x", NULL, NULL, NULL, NULL};
        if (snes_rom_revision_matches(&none, good_sha, "", ""))
            fail("digest-less entry matched", NULL);
    }

    puts("rom_identity_test: PASS");
    return 0;
}
