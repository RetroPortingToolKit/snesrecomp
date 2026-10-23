/* Storage microbenchmark, not a game/host throughput qualification.
 * Run independent processes in balanced order; no timing assertions. */
#include "raster_snapshot.h"
#include <stdio.h>
#include <time.h>

static uint16_t source[0x8000];
static volatile uint64_t checksum;
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: raster_snapshot_benchmark reuse(0|1) pattern(0..2) frames\n");
        return 2;
    }
    unsigned reuse = (unsigned)strtoul(argv[1], NULL, 10);
    unsigned pattern = (unsigned)strtoul(argv[2], NULL, 10);
    unsigned frames = (unsigned)strtoul(argv[3], NULL, 10);
    if (reuse > 1 || pattern > 2 || !frames || frames > 100000) return 2;
    SnesRasterSnapshotStore store = {0};
    const uint16_t *lines[224];
    uint64_t copied = 0;
    clock_t start = 0;
    for (unsigned frame = 0; frame < frames + 16; ++frame) {
        if (frame == 16) start = clock();
        SnesRasterSnapshotBegin(&store, sizeof(source), 224);
        memset(source, (int)(frame & 255), sizeof(source));
        for (unsigned line = 0; line < 224; ++line) {
            if (pattern == 1 && line % 8 == 0) ++source[0x7fff];
            if (pattern == 2) ++source[(line * 977) & 0x7fff];
            lines[line] = SnesRasterSnapshotCapture(&store, source, reuse != 0);
            if (!lines[line]) return 1;
        }
        for (unsigned line = 0; line < 224; ++line)
            checksum += lines[line][(line * 977) & 0x7fff];
        if (frame >= 16) copied += store.copied_bytes;
    }
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("{\"reuse\":%u,\"pattern\":%u,\"frames\":%u,\"seconds\":%.6f,"
           "\"copied_bytes\":%llu,\"allocated_bytes\":%zu,\"versions\":%u,"
           "\"checksum\":%llu}\n", reuse, pattern, frames, seconds,
           (unsigned long long)copied, store.allocated * store.image_bytes,
           store.count, (unsigned long long)checksum);
    SnesRasterSnapshotDestroy(&store);
    return 0;
}
