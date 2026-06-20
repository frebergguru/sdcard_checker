// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Raw-device test (DESTRUCTIVE, needs root).
 *
 * Safety gate first: must be confirmed, must be root, must be a removable
 * device, and must not be mounted. Then a fast "quick" probe samples blocks
 * across the whole announced capacity (write all, then read all, in increasing
 * offset order) to find where real storage ends, or an optional full-surface
 * sequential write+verify for a thorough pass.
 *
 * Fake cards that DROP writes past their real size read back as zeros/garbage,
 * so the first failing sample marks the real boundary. Cards that WRAP (alias
 * high addresses onto low ones) corrupt low samples when the high writes land,
 * which still yields a FAKE verdict; the capacity estimate is then the largest
 * intact prefix and is reported as approximate.
 */
#define _GNU_SOURCE
#include "sdcheck/sdcheck.h"
#include "pattern.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>

#define BLK   (1024u * 1024)        /* sample/probe block size           */
#define CHUNK (4u * 1024 * 1024)    /* full-surface I/O buffer           */
#define MAX_SAMPLES 4096

static int fail(sdcheck_result *r, const char *msg)
{
    r->verdict = SDCHECK_ERROR;
    snprintf(r->message, sizeof(r->message), "%s", msg);
    return -1;
}

/* Build the sorted, de-duplicated list of sample offsets to probe. */
static int build_samples(uint64_t announced, uint64_t *offs, int max)
{
    int n = 0;
    if (announced < BLK) return 0;
    uint64_t last = announced - BLK;
    uint64_t step = announced / 1024;
    if (step < BLK) step = BLK;
    step = (step / BLK) * BLK;
    if (step == 0) step = BLK;

    for (uint64_t o = 0; o <= last && n < max - 1; o += step)
        offs[n++] = o;
    offs[n++] = last;              /* always probe the very last block */
    return n;
}

/* Append one contiguous bad region to the result's bad-block map. */
static void record_bad(sdcheck_result *r, uint64_t off, uint64_t len)
{
    if (len == 0) return;
    if (r->bad_region_count < SDCHECK_MAX_BAD_REGIONS)
        r->bad_regions[r->bad_region_count++] = (sdcheck_bad_region){ off, len };
    else
        r->bad_regions_truncated = 1;
}

/* splitmix64 step, for random IOPS offsets. */
static inline uint64_t splitmix_step(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

#define IOPS_BS       4096u
#define IOPS_SECONDS  2.0       /* per direction */

/*
 * Random 4 KiB IOPS benchmark (SD App Performance Class A1/A2). DESTRUCTIVE:
 * writes random blocks. Opens its own O_DIRECT fd to bypass the page cache (so
 * the numbers reflect the card); falls back to a buffered fd where O_DIRECT is
 * unsupported (e.g. a tmpfs-backed virtual device). Fills read/write_iops and
 * app_class; silently does nothing if it can't run.
 */
static void measure_iops(const char *devnode, uint64_t size,
                         sdcheck_progress_cb cb, void *user_data,
                         volatile int *cancel, sdcheck_result *result)
{
    if (size < IOPS_BS * 2ULL) return;

    int fd = open(devnode, O_RDWR | O_DIRECT | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) fd = open(devnode, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;

    void *buf = NULL;
    if (posix_memalign(&buf, IOPS_BS, IOPS_BS) != 0) { close(fd); return; }
    memset(buf, 0x5A, IOPS_BS);

    uint64_t nblk = size / IOPS_BS;
    uint64_t s = 0xA5A5C0FFEEULL ^ size;

    /* random writes */
    if (cb) cb(SDCHECK_PHASE_IOPS, 0, 2, 0.0, user_data);
    uint64_t wops = 0;
    double t0 = sdcheck_now();
    while (sdcheck_now() - t0 < IOPS_SECONDS) {
        if (cancel && *cancel) break;
        off_t off = (off_t)((splitmix_step(&s) % nblk) * IOPS_BS);
        if (pwrite(fd, buf, IOPS_BS, off) == (ssize_t)IOPS_BS) wops++;
    }
    double wsec = sdcheck_now() - t0;
    result->write_iops = wsec > 0.0 ? (double)wops / wsec : 0.0;
    fdatasync(fd);

    /* random reads */
    if (cb) cb(SDCHECK_PHASE_IOPS, 1, 2, 0.0, user_data);
    uint64_t rops = 0;
    t0 = sdcheck_now();
    while (sdcheck_now() - t0 < IOPS_SECONDS) {
        if (cancel && *cancel) break;
        off_t off = (off_t)((splitmix_step(&s) % nblk) * IOPS_BS);
        if (pread(fd, buf, IOPS_BS, off) == (ssize_t)IOPS_BS) rops++;
    }
    double rsec = sdcheck_now() - t0;
    result->read_iops = rsec > 0.0 ? (double)rops / rsec : 0.0;

    result->app_class = sdcheck_app_class_from_iops(result->read_iops,
                                                    result->write_iops);
    free(buf);
    close(fd);
    if (cb) cb(SDCHECK_PHASE_IOPS, 2, 2, 0.0, user_data);
}

static int probe_quick(int fd, uint64_t announced, uint64_t seed,
                       sdcheck_progress_cb cb, void *user_data,
                       volatile int *cancel, sdcheck_result *result)
{
    static uint64_t offs[MAX_SAMPLES];
    int nsamp = build_samples(announced, offs, MAX_SAMPLES);
    if (nsamp <= 0) return fail(result, "Device too small to probe.");

    unsigned char *buf = malloc(BLK);
    unsigned char * exp = malloc(BLK);
    if (!buf || !exp) { free(buf); free(exp); return fail(result, "Out of memory."); }

    sdcheck_rate wrate, rrate;
    sdcheck_rate_init(&wrate);
    sdcheck_rate_init(&rrate);

    /* ---- write all samples in increasing offset order ---- */
    if (cb) cb(SDCHECK_PHASE_WRITE, 0, (uint64_t)nsamp, 0.0, user_data);
    uint64_t since_sync = 0;       /* bytes written since last fdatasync */
    for (int i = 0; i < nsamp; i++) {
        if (cancel && *cancel) { free(buf); free(exp); snprintf(result->message, sizeof(result->message), "Cancelled."); return 0; }
        pattern_fill(buf, BLK, seed, offs[i]);
        if (lseek(fd, (off_t)offs[i], SEEK_SET) < 0) { free(buf); free(exp); return fail(result, "Seek failed."); }
        double t0 = sdcheck_now();
        ssize_t w = write(fd, buf, BLK);
        if (w > 0) {
            since_sync += (uint64_t)w;
            if (since_sync >= SDCHECK_RATE_WINDOW) {
                fdatasync(fd);
                since_sync = 0;
            }
        }
        double dt = sdcheck_now() - t0;
        if (w < 0) { free(buf); free(exp); return fail(result, "Write failed (device error)."); }
        sdcheck_rate_add(&wrate, (uint64_t)w, dt);
        result->bytes_written += (uint64_t)w;
        if (cb) cb(SDCHECK_PHASE_WRITE, (uint64_t)(i + 1), (uint64_t)nsamp,
                   sdcheck_rate_live(&wrate), user_data);
    }
    fsync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);  /* force reads from media */

    /* ---- read all samples back, classify good/bad ---- */
    if (cb) cb(SDCHECK_PHASE_PROBE, 0, (uint64_t)nsamp, 0.0, user_data);
    uint64_t good_prefix_end = 0;   /* end offset of contiguous good run from 0 */
    int prefix_broken = 0;
    long bad_count = 0;
    for (int i = 0; i < nsamp; i++) {
        if (cancel && *cancel) { free(buf); free(exp); snprintf(result->message, sizeof(result->message), "Cancelled."); return 0; }
        if (lseek(fd, (off_t)offs[i], SEEK_SET) < 0) { free(buf); free(exp); return fail(result, "Seek failed."); }
        double t0 = sdcheck_now();
        ssize_t r = read(fd, buf, BLK);
        double dt = sdcheck_now() - t0;
        if (r <= 0) { free(buf); free(exp); return fail(result, "Read failed (device error)."); }
        sdcheck_rate_add(&rrate, (uint64_t)r, dt);
        result->bytes_verified += (uint64_t)r;

        pattern_fill(exp, (size_t)r, seed, offs[i]);
        int ok = (memcmp(buf, exp, (size_t)r) == 0);
        if (ok) {
            if (!prefix_broken) good_prefix_end = offs[i] + (uint64_t)r;
        } else {
            bad_count++;
            result->bad_bytes += (uint64_t)r;
            if (!prefix_broken) prefix_broken = 1;
        }
        if (cb) cb(SDCHECK_PHASE_PROBE, (uint64_t)(i + 1), (uint64_t)nsamp,
                   sdcheck_rate_live(&rrate), user_data);
    }

    sdcheck_rate_finish(&wrate);
    sdcheck_rate_finish(&rrate);
    result->announced_bytes = announced;
    result->avg_write_bps = sdcheck_rate_avg(&wrate);
    result->min_write_bps = sdcheck_rate_min(&wrate);
    result->max_write_bps = sdcheck_rate_max(&wrate);
    result->avg_read_bps  = sdcheck_rate_avg(&rrate);
    result->min_read_bps  = sdcheck_rate_min(&rrate);
    result->max_read_bps  = sdcheck_rate_max(&rrate);
    result->speed_class   = sdcheck_speed_class_from_bps(result->avg_write_bps);

    if (bad_count == 0) {
        result->verdict = SDCHECK_GENUINE;
        result->real_bytes = announced;
        snprintf(result->message, sizeof(result->message),
                 "GENUINE: all %d samples across %.2f GB verified intact.",
                 nsamp, (double)announced / 1e9);
    } else {
        result->verdict = SDCHECK_FAKE;
        result->real_bytes = good_prefix_end;
        snprintf(result->message, sizeof(result->message),
                 "FAKE: %ld of %d samples corrupted; real capacity ~%.2f GB of claimed %.2f GB.",
                 bad_count, nsamp, (double)good_prefix_end / 1e9, (double)announced / 1e9);
    }

    free(buf);
    free(exp);
    return 0;
}

static int probe_full(int fd, uint64_t announced, uint64_t seed,
                      sdcheck_progress_cb cb, void *user_data,
                      volatile int *cancel, sdcheck_result *result)
{
    unsigned char *buf = malloc(CHUNK);
    unsigned char *exp = malloc(CHUNK);
    if (!buf || !exp) { free(buf); free(exp); return fail(result, "Out of memory."); }

    sdcheck_rate wrate, rrate;
    sdcheck_rate_init(&wrate);
    sdcheck_rate_init(&rrate);

    /* ---- sequential write ---- */
    if (cb) cb(SDCHECK_PHASE_WRITE, 0, announced, 0.0, user_data);
    lseek(fd, 0, SEEK_SET);
    uint64_t since_sync = 0;       /* bytes written since last fdatasync */
    for (uint64_t off = 0; off < announced; ) {
        if (cancel && *cancel) { free(buf); free(exp); snprintf(result->message, sizeof(result->message), "Cancelled."); return 0; }
        size_t want = (size_t)((announced - off < CHUNK) ? (announced - off) : CHUNK);
        pattern_fill(buf, want, seed, off);
        double t0 = sdcheck_now();
        ssize_t w = write(fd, buf, want);
        if (w > 0) {
            /* Flush a window's worth to media before stopping the clock so the
               rate is the card's, not the page cache's. */
            since_sync += (uint64_t)w;
            if (since_sync >= SDCHECK_RATE_WINDOW) {
                fdatasync(fd);
                since_sync = 0;
            }
        }
        double dt = sdcheck_now() - t0;
        if (w <= 0) { free(buf); free(exp); return fail(result, "Write failed during full pass."); }
        sdcheck_rate_add(&wrate, (uint64_t)w, dt);
        result->bytes_written += (uint64_t)w;
        off += (uint64_t)w;
        if (cb) cb(SDCHECK_PHASE_WRITE, off, announced, sdcheck_rate_live(&wrate), user_data);
    }
    fsync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

    /* ---- sequential read/verify (builds the bad-block map) ---- */
    if (cb) cb(SDCHECK_PHASE_READ, 0, announced, 0.0, user_data);
    lseek(fd, 0, SEEK_SET);
    long first_bad = -1;
    uint64_t bad_start = 0;    /* current contiguous bad run */
    int in_bad = 0;
    uint64_t pos = 0;
    for (uint64_t off = 0; off < announced; ) {
        if (cancel && *cancel) { free(buf); free(exp); snprintf(result->message, sizeof(result->message), "Cancelled."); return 0; }
        size_t want = (size_t)((announced - off < CHUNK) ? (announced - off) : CHUNK);
        double t0 = sdcheck_now();
        ssize_t r = read(fd, buf, want);
        double dt = sdcheck_now() - t0;
        if (r <= 0) { free(buf); free(exp); return fail(result, "Read failed during full pass."); }
        sdcheck_rate_add(&rrate, (uint64_t)r, dt);
        pattern_fill(exp, (size_t)r, seed, off);
        for (ssize_t i = 0; i < r; i++) {
            pos = off + (uint64_t)i;
            if (buf[i] != exp[i]) {
                result->bad_bytes++;
                if (first_bad < 0) first_bad = (long)pos;
                if (!in_bad) { in_bad = 1; bad_start = pos; }
            } else if (in_bad) {
                record_bad(result, bad_start, pos - bad_start);
                in_bad = 0;
            }
        }
        result->bytes_verified += (uint64_t)r;
        off += (uint64_t)r;
        if (cb) cb(SDCHECK_PHASE_READ, off, announced, sdcheck_rate_live(&rrate), user_data);
    }
    if (in_bad) record_bad(result, bad_start, (pos + 1) - bad_start);

    sdcheck_rate_finish(&wrate);
    sdcheck_rate_finish(&rrate);
    result->announced_bytes = announced;
    result->avg_write_bps = sdcheck_rate_avg(&wrate);
    result->min_write_bps = sdcheck_rate_min(&wrate);
    result->max_write_bps = sdcheck_rate_max(&wrate);
    result->avg_read_bps  = sdcheck_rate_avg(&rrate);
    result->min_read_bps  = sdcheck_rate_min(&rrate);
    result->max_read_bps  = sdcheck_rate_max(&rrate);
    result->speed_class   = sdcheck_speed_class_from_bps(result->avg_write_bps);

    if (result->bad_bytes == 0) {
        result->verdict = SDCHECK_GENUINE;
        result->real_bytes = announced;
        snprintf(result->message, sizeof(result->message),
                 "GENUINE: full %.2f GB written and verified with no errors.",
                 (double)announced / 1e9);
    } else {
        result->verdict = SDCHECK_FAKE;
        result->real_bytes = first_bad >= 0 ? (uint64_t)first_bad : 0;
        snprintf(result->message, sizeof(result->message),
                 "FAKE: first error at %.2f GB; real capacity ~%.2f GB of claimed %.2f GB.",
                 (double)result->real_bytes / 1e9, (double)result->real_bytes / 1e9,
                 (double)announced / 1e9);
    }

    free(buf);
    free(exp);
    return 0;
}

int sdcheck_run_device(const char *devnode,
                       const sdcheck_options *opt,
                       sdcheck_progress_cb cb,
                       void *user_data,
                       volatile int *cancel,
                       sdcheck_result *result)
{
    sdcheck_options def;
    if (!opt) { sdcheck_options_default(&def); opt = &def; }
    memset(result, 0, sizeof(*result));
    result->verdict = SDCHECK_ERROR;

    /* ---- safety gate (shared) ---- */
    if (sdcheck_guard_destructive(devnode, opt->confirm_destroy, result) != 0)
        return -1;

    int fd = open(devnode, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return fail(result, "Cannot open device (need root / correct path).");

    uint64_t announced = 0;
    if (ioctl(fd, BLKGETSIZE64, &announced) != 0 || announced == 0) {
        /* Regular-file virtual device: use its size. */
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
            announced = (uint64_t)st.st_size;
        else {
            close(fd);
            return fail(result, "Cannot determine device size.");
        }
    }
    if (announced > SDCHECK_MAX_SANE_BYTES) {
        close(fd);
        return fail(result,
            "Device reports an implausible capacity (over 64 TB) — most likely a "
            "flaky reader or a fake. Re-seat the card, or use a native SD slot.");
    }

    int passes = opt->passes > 0 ? opt->passes : 1;
    int rc = 0;

    /* Multi-pass stress: repeat the write+verify, fresh pattern each pass, and
       stop at the first pass that isn't GENUINE. */
    for (int p = 1; p <= passes; p++) {
        memset(result, 0, sizeof(*result));
        result->verdict = SDCHECK_ERROR;
        uint64_t seed = opt->seed ? opt->seed : sdcheck_random_seed();

        rc = opt->full_surface
            ? probe_full(fd, announced, seed, cb, user_data, cancel, result)
            : probe_quick(fd, announced, seed, cb, user_data, cancel, result);

        result->passes_done = p;
        result->passes_total = passes;

        if (passes > 1 && result->verdict == SDCHECK_GENUINE)
            snprintf(result->message + strlen(result->message),
                     sizeof(result->message) - strlen(result->message),
                     " (pass %d/%d)", p, passes);

        if (rc != 0 || result->verdict != SDCHECK_GENUINE) break;
        if (cancel && *cancel) break;
    }

    /* Optional random-IOPS benchmark on a card that passed. */
    if (opt->measure_iops && rc == 0 && result->verdict == SDCHECK_GENUINE &&
        !(cancel && *cancel))
        measure_iops(devnode, announced, cb, user_data, cancel, result);

    if (cb) cb(SDCHECK_PHASE_DONE, 0, 0, 0.0, user_data);
    close(fd);
    return rc;
}
