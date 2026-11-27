#include "sha256.h"
#include <recomp_mod.h>

void sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]) {
    (void)rmod_sha256_bytes(data, len, out);
}
