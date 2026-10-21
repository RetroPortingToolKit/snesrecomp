#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Single simulation-thread producer, one file-writing consumer. The bounded
 * queue never waits for the filesystem; overflow stops this optional trace.
 * Close drains accepted rows and joins the writer outside the frame loop. */
typedef struct SnesTraceWriter SnesTraceWriter;
SnesTraceWriter *SnesTraceWriterOpen(const char *stem, const char *header,
                                           size_t segment_bytes, unsigned keep);
bool SnesTraceWriterWrite(SnesTraceWriter *writer, const char *row, size_t size);
const char *SnesTraceWriterError(SnesTraceWriter *writer);
/* Return a static error description, or NULL after a successful drain. */
const char *SnesTraceWriterClose(SnesTraceWriter *writer);
#ifdef __cplusplus
}
#endif
