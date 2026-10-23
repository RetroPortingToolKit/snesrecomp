#ifndef SNESRECOMP_RASTER_SNAPSHOT_H
#define SNESRECOMP_RASTER_SNAPSHOT_H

/* Immutable, host-only memory images for a raster frame. A returned image is
 * valid until the next Begin/Destroy on this store. Never put these pointers,
 * allocation history or counters in guest snapshots or determinism digests.
 *
 * Compare bytes rather than a hash or an assumed write epoch: CPU, DMA, host
 * patches and restored memory can all change the source without notification.
 * Consecutive equal images share storage; changed images own separate malloc
 * blocks so growing the store cannot invalidate an earlier scanline. Blocks
 * are reused across frames, avoiding steady-state allocation. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct SnesRasterSnapshotStore {
    void **images;
    size_t image_bytes;
    unsigned count, allocated, limit;
    uint64_t copied_bytes, compared_bytes;
} SnesRasterSnapshotStore;

/* Zero-initialize a store before its first use. */
static inline void SnesRasterSnapshotDestroy(SnesRasterSnapshotStore *store) {
    if (!store) return;
    for (unsigned i = 0; i < store->allocated; ++i) free(store->images[i]);
    free(store->images);
    memset(store, 0, sizeof(*store));
}

static inline bool SnesRasterSnapshotBegin(SnesRasterSnapshotStore *store,
                                          size_t image_bytes, unsigned limit) {
    if (!store || !image_bytes || !limit) return false;
    if (store->image_bytes != image_bytes || store->limit != limit)
        SnesRasterSnapshotDestroy(store);
    if (!store->images) {
        store->images = (void **)calloc(limit, sizeof(*store->images));
        if (!store->images) return false;
    }
    store->image_bytes = image_bytes;
    store->limit = limit;
    store->count = 0;
    store->copied_bytes = store->compared_bytes = 0;
    return true;
}

/* reuse_equal=false is the full-copy correctness/performance reference.
 * Failure leaves existing images intact; the caller must reject the incomplete
 * frame and use its normal presentation fallback. No guest work is skipped. */
static inline const void *SnesRasterSnapshotCapture(
        SnesRasterSnapshotStore *store, const void *source, bool reuse_equal) {
    if (!store || !source || !store->image_bytes) return NULL;
    if (reuse_equal && store->count) {
        const void *previous = store->images[store->count - 1];
        store->compared_bytes += store->image_bytes;
        if (!memcmp(previous, source, store->image_bytes)) return previous;
    }
    if (store->count == store->limit) return NULL;
    if (store->count == store->allocated) {
        void *image = malloc(store->image_bytes);
        if (!image) return NULL;
        store->images[store->allocated++] = image;
    }
    void *image = store->images[store->count++];
    memcpy(image, source, store->image_bytes);
    store->copied_bytes += store->image_bytes;
    return image;
}

#endif
