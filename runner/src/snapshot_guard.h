#ifndef SNES_SNAPSHOT_GUARD_H
#define SNES_SNAPSHOT_GUARD_H
#include <stdbool.h>
#include <stddef.h>

/* An opt-in envelope around the unchanged RTLS stream. The identity is a
 * versioned game/mode contract (at most 31 bytes), not a filename. */
#define SNES_SNAPSHOT_GUARD_PREFIX 48u
#define SNES_SNAPSHOT_GUARD_OVERHEAD 52u
bool snes_snapshot_guard_identity_valid(const char *identity);
/* Payload has already been written after PREFIX bytes. NULL data counts size. */
size_t snes_snapshot_guard_finish(void *data, size_t capacity,
    size_t payload_size, const char *identity);
bool snes_snapshot_guard_open(const void *data, size_t size,
    const char *expected_identity, const void **payload, size_t *payload_size);
#endif
