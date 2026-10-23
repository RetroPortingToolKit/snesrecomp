#include <stdlib.h>
static int allocation_budget = -1;
static void *test_malloc(size_t bytes) {
    if (!allocation_budget) return NULL;
    if (allocation_budget > 0) --allocation_budget;
    return malloc(bytes);
}
static void *test_calloc(size_t count, size_t bytes) {
    if (!allocation_budget) return NULL;
    if (allocation_budget > 0) --allocation_budget;
    return calloc(count, bytes);
}
#define malloc test_malloc
#define calloc test_calloc
#include "raster_snapshot.h"
#undef malloc
#undef calloc
#include <assert.h>
#include <stdio.h>

enum { WORDS = 0x8000, LINES = 224 };
static uint16_t source[WORDS], oracle[LINES][WORDS];
static const uint16_t *captured[LINES];

static void compare_frame(SnesRasterSnapshotStore *store, unsigned pattern,
                          bool reuse) {
    assert(SnesRasterSnapshotBegin(store, sizeof(source), LINES));
    memset(source, 0, sizeof(source));
    for (unsigned line = 0; line < LINES; ++line) {
        if (pattern == 1 && line % 7 == 0) source[WORDS - 1]++;
        if (pattern == 2) { source[0] = line & 1; source[WORDS - 1] = line; }
        if (pattern == 3) source[WORDS / 2] = (line / 3) & 1; /* ABA */
        if (pattern == 4) { /* Restored/directly edited state, no write hook. */
            memset(source, line % 11, sizeof(source));
            source[(line * 977) % WORDS] ^= 0xa55a;
        }
        memcpy(oracle[line], source, sizeof(source));
        captured[line] = SnesRasterSnapshotCapture(store, source, reuse);
        assert(captured[line]);
    }
    /* Later guest mutations must never retroactively edit a captured line. */
    memset(source, 0xfe, sizeof(source));
    for (unsigned line = 0; line < LINES; ++line)
        assert(!memcmp(captured[line], oracle[line], sizeof(source)));
    if (!pattern) {
        assert(store->count == (reuse ? 1 : LINES));
        assert(store->copied_bytes == (uint64_t)sizeof(source) * (reuse ? 1 : LINES));
        if (reuse) for (unsigned line = 1; line < LINES; ++line)
            assert(captured[line] == captured[0]);
    }
}

int main(void) {
    SnesRasterSnapshotStore store = {0};
    assert(!SnesRasterSnapshotBegin(NULL, sizeof(source), LINES));
    assert(!SnesRasterSnapshotBegin(&store, 0, LINES));
    assert(!SnesRasterSnapshotBegin(&store, sizeof(source), 0));
    assert(!SnesRasterSnapshotCapture(&store, source, true));
    for (unsigned pass = 0; pass < 3; ++pass)
        for (unsigned pattern = 0; pattern < 5; ++pattern)
            for (unsigned reuse = 0; reuse < 2; ++reuse)
                compare_frame(&store, pattern, reuse != 0);
    assert(!SnesRasterSnapshotCapture(&store, NULL, true));
    /* The final pattern changes every line; all immutable slots are occupied. */
    assert(store.count == LINES);
    assert(!SnesRasterSnapshotCapture(&store, source, false));
    for (unsigned line = 0; line < LINES; ++line)
        assert(!memcmp(captured[line], oracle[line], sizeof(source)));
    /* Size changes discard the old allocation shape, not guest state. */
    assert(SnesRasterSnapshotBegin(&store, 7, 3));
    assert(store.allocated == 0 && store.count == 0);
    assert(SnesRasterSnapshotCapture(&store, "abcdefg", true));
    assert(store.allocated == 1 && store.copied_bytes == 7);
    SnesRasterSnapshotDestroy(&store);
    assert(!store.allocated && !store.image_bytes && !store.images);
    SnesRasterSnapshotDestroy(&store);
    allocation_budget = 0;
    assert(!SnesRasterSnapshotBegin(&store, sizeof(source), LINES));
    allocation_budget = 1;
    assert(SnesRasterSnapshotBegin(&store, sizeof(source), LINES));
    assert(!SnesRasterSnapshotCapture(&store, source, true));
    assert(!store.count && !store.allocated);
    allocation_budget = -1;
    const void *first = SnesRasterSnapshotCapture(&store, source, true);
    assert(first);
    memcpy(oracle[0], source, sizeof(source));
    ++source[0];
    allocation_budget = 0;
    assert(!SnesRasterSnapshotCapture(&store, source, true));
    assert(store.count == 1 && !memcmp(first, oracle[0], sizeof(source)));
    SnesRasterSnapshotDestroy(&store);
    puts("raster snapshots match full-copy oracle across mutation and restore patterns");
    return 0;
}
