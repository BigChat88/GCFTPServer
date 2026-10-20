#ifndef GCFTP_XFER_H
#define GCFTP_XFER_H

/*
 * Bounded producer/consumer pipeline used for file transfers, so disk I/O
 * and network I/O overlap instead of taking turns. libogc2's lwIP only
 * advertises a 2-segment receive window that reopens when the application
 * reads, so an upload stalls whenever the reader is busy writing to the SD.
 */

#include <gctypes.h>
#include <stdbool.h>

#define XFER_NBUF  4
#define XFER_CHUNK (64 * 1024)

/* Fills buf with up to cap bytes: >0 data, 0 end of stream, <0 error */
typedef s32 (*xfer_produce_fn)(void *ctx, u8 *buf, s32 cap);
/* Consumes len bytes; false on error */
typedef bool (*xfer_consume_fn)(void *ctx, const u8 *buf, s32 len);

/* Runs one side in a lower-priority worker thread (the disk side) and the
 * other on the calling thread. Returns bytes consumed, or -1 on failure. */
s64 xfer_run(u8 *bufs[XFER_NBUF],
             xfer_produce_fn produce, void *produce_ctx,
             xfer_consume_fn consume, void *consume_ctx,
             bool worker_produces);

#endif
