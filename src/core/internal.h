/* Internal helpers shared across the core library (not part of public API). */
#ifndef SDCHECK_INTERNAL_H
#define SDCHECK_INTERNAL_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>

#include "sdcheck/sdcheck.h"

/*
 * A fresh, unpredictable 64-bit seed for the write pattern. Used when the caller
 * does not pin a seed, so every run writes different data across the card — a
 * counterfeit can't pre-store the expected bytes or echo back a known/constant
 * pattern (zeros, 0xFF, or its own "random" filler) and pass. Falls back to
 * /dev/urandom, then to a fixed non-zero constant if both are unavailable.
 */
static inline uint64_t sdcheck_random_seed(void)
{
    uint64_t s = 0;
    if (getrandom(&s, sizeof(s), 0) == (ssize_t)sizeof(s) && s != 0)
        return s;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t n = fread(&s, 1, sizeof(s), f);
        fclose(f);
        if (n == sizeof(s) && s != 0)
            return s;
    }
    return 0x5DCA2DC4EC6E5A1DULL;
}

/*
 * Shared destructive-operation safety gate (defined in device_guard.c).
 * Verifies: confirmation given, running as root, target is a whole-disk
 * removable device; if it (or its partitions) are mounted, they are unmounted
 * automatically. On failure it fills result->message and
 * result->verdict = SDCHECK_ERROR and returns -1.
 * Returns 0 if it is safe to write to devnode.
 */
int sdcheck_guard_destructive(const char *devnode, int confirm,
                              sdcheck_result *result);

/* Monotonic wall-clock seconds, for throughput timing. */
static inline double sdcheck_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * Throughput window size. Individual I/O chunks (a few MB) are far too small to
 * time reliably: a single chunk can be absorbed by the page/device cache and
 * clock in at memcpy speed (tens of GB/s), or catch a writeback stall and clock
 * in near zero. Neither extreme reflects the card. So we aggregate raw chunks
 * into windows of at least this many bytes and derive one rate per window; the
 * reported min/max are over those windows, not over single chunks. Callers that
 * want durable write rates should fdatasync once per window (see filemode etc.)
 * so the window's time includes the flush, not just the copy into cache.
 */
#define SDCHECK_RATE_WINDOW (32u * 1024 * 1024)

/*
 * Upper sanity bound on a reported block-device capacity. The largest real
 * SD/USB flash is a couple of TB; a value far above this means the kernel got a
 * bogus size from the device — a flaky USB reader or a fake reporting a maximal
 * LBA. Acting on it would be nonsense (a "capacity" of tens of TB, a test that
 * would run for days), so the destructive entry points reject it up front.
 */
#define SDCHECK_MAX_SANE_BYTES (64ULL * 1024 * 1024 * 1024 * 1024)  /* 64 TiB */

/*
 * Running throughput tracker. Feed it (bytes, seconds) samples for individual
 * chunks; it accumulates totals for the average and folds chunks into windows
 * to track a robust min/max sustained rate. Call sdcheck_rate_finish() once
 * after the last chunk to flush the trailing partial window.
 */
typedef struct {
    uint64_t total_bytes;
    double   total_secs;
    /* current aggregation window, flushed once it reaches SDCHECK_RATE_WINDOW */
    uint64_t win_bytes;
    double   win_secs;
    /* min/max over completed windows */
    double   min_bps;
    double   max_bps;
    double   last_bps;   /* rate of the most recently completed window  */
    int      have_window;
} sdcheck_rate;

static inline void sdcheck_rate_init(sdcheck_rate *r)
{
    r->total_bytes = 0;
    r->total_secs = 0.0;
    r->win_bytes = 0;
    r->win_secs = 0.0;
    r->min_bps = 0.0;
    r->max_bps = 0.0;
    r->last_bps = 0.0;
    r->have_window = 0;
}

/* Turn the accumulated window into one min/max sample and reset it. */
static inline void sdcheck_rate_flush_window(sdcheck_rate *r)
{
    if (r->win_bytes == 0 || r->win_secs <= 1e-6) {
        r->win_bytes = 0;
        r->win_secs = 0.0;
        return;
    }
    double bps = (double)r->win_bytes / r->win_secs;
    r->last_bps = bps;
    if (!r->have_window) {
        r->min_bps = r->max_bps = bps;
        r->have_window = 1;
    } else {
        if (bps < r->min_bps) r->min_bps = bps;
        if (bps > r->max_bps) r->max_bps = bps;
    }
    r->win_bytes = 0;
    r->win_secs = 0.0;
}

static inline void sdcheck_rate_add(sdcheck_rate *r, uint64_t bytes, double secs)
{
    r->total_bytes += bytes;
    r->total_secs += secs;
    r->win_bytes += bytes;
    r->win_secs += secs;
    if (r->win_bytes >= SDCHECK_RATE_WINDOW)
        sdcheck_rate_flush_window(r);
}

/* Flush any trailing partial window so short tests still report a min/max. */
static inline void sdcheck_rate_finish(sdcheck_rate *r)
{
    sdcheck_rate_flush_window(r);
}

static inline double sdcheck_rate_avg(const sdcheck_rate *r)
{
    return r->total_secs > 1e-9 ? (double)r->total_bytes / r->total_secs : 0.0;
}

/*
 * A smoothed rate for the live progress readout: the most recently completed
 * window (which spans >=32 MiB and includes its periodic flush, so it reflects
 * real media throughput rather than single-chunk page-cache speed). Falls back
 * to the cumulative average until the first window completes.
 */
static inline double sdcheck_rate_live(const sdcheck_rate *r)
{
    return r->have_window ? r->last_bps : sdcheck_rate_avg(r);
}

#endif /* SDCHECK_INTERNAL_H */
