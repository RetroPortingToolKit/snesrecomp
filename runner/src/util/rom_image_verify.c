#include "rom_image_verify.h"

#include "crc32.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

typedef struct RomImage {
    uint8_t *storage;
    const uint8_t *payload;
    size_t payload_size;
} RomImage;

/* Load a ROM once and expose its headerless payload. A file whose size is 512
 * bytes beyond a 1 KiB boundary is treated as carrying an SMC copier header,
 * matching the launcher's historical behavior. */
static int rom_image_load(const char *path, RomImage *image) {
    memset(image, 0, sizeof(*image));
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Launcher] Cannot open '%s'\n", path);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long raw_size = ftell(f);
    if (raw_size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    image->storage = (uint8_t *)malloc((size_t)raw_size);
    if (!image->storage) {
        fclose(f);
        return 0;
    }
    size_t read = fread(image->storage, 1, (size_t)raw_size, f);
    fclose(f);
    if (read != (size_t)raw_size) {
        free(image->storage);
        memset(image, 0, sizeof(*image));
        return 0;
    }

    size_t header_size = ((size_t)raw_size % 1024 == 512) ? 512 : 0;
    image->payload = image->storage + header_size;
    image->payload_size = (size_t)raw_size - header_size;
    return 1;
}

static void rom_image_free(RomImage *image) {
    free(image->storage);
    memset(image, 0, sizeof(*image));
}

static void hash_to_hex(const uint8_t hash[32], char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        hex[i * 2] = digits[hash[i] >> 4];
        hex[i * 2 + 1] = digits[hash[i] & 0x0f];
    }
    hex[64] = '\0';
}

int snesrecomp_rom_is_readable(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int snesrecomp_rom_verify_crc32(const char *path, uint32_t expected_crc) {
    if (expected_crc == 0) return 1;

    RomImage image;
    if (!rom_image_load(path, &image)) return 0;
    uint32_t actual = crc32_compute(image.payload, image.payload_size);
    rom_image_free(&image);
    if (actual == expected_crc) return 1;

    char msg[256];
    snprintf(msg, sizeof(msg),
             "ROM CRC32 mismatch.\n\nExpected: %08X\nGot:      %08X\n\n"
             "Please select the correct ROM file.",
             expected_crc, actual);
    fprintf(stderr, "[Launcher] %s\n", msg);
#ifdef _WIN32
    MessageBoxA(NULL, msg, "Wrong ROM", MB_ICONWARNING | MB_OK);
#endif
    return 0;
}

int snesrecomp_rom_verify_sha256(const char *path,
                                 const uint8_t *expected_sha256) {
    if (!expected_sha256) return 1;

    RomImage image;
    if (!rom_image_load(path, &image)) return 0;
    uint8_t actual[32];
    sha256_compute(image.payload, image.payload_size, actual);
    rom_image_free(&image);
    if (memcmp(actual, expected_sha256, sizeof(actual)) == 0) return 1;

    char expected_hex[65];
    char actual_hex[65];
    hash_to_hex(expected_sha256, expected_hex);
    hash_to_hex(actual, actual_hex);
    char msg[512];
    snprintf(msg, sizeof(msg),
             "ROM SHA-256 mismatch.\n\nExpected:\n%s\n\nGot:\n%s\n\n"
             "Please select the correct ROM file.",
             expected_hex, actual_hex);
    fprintf(stderr, "[Launcher] %s\n", msg);
#ifdef _WIN32
    MessageBoxA(NULL, msg, "Wrong ROM", MB_ICONWARNING | MB_OK);
#endif
    return 0;
}

int snesrecomp_rom_match_sha256(const char *path,
                                const uint8_t (*hashes)[32],
                                size_t n_hashes) {
    RomImage image;
    if (!rom_image_load(path, &image)) return -1;
    uint8_t actual[32];
    sha256_compute(image.payload, image.payload_size, actual);
    rom_image_free(&image);

    for (size_t i = 0; i < n_hashes; i++)
        if (memcmp(actual, hashes[i], sizeof(actual)) == 0)
            return (int)i;

    char hex[65];
    hash_to_hex(actual, hex);
    fprintf(stderr,
            "[Launcher] ROM SHA-256 %s matches no known hash.\n", hex);
    return -1;
}
