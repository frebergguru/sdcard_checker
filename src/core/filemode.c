// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * File-based test (safe, no root): write sequential pattern files into a
 * mounted card's free space until full, then read them back and verify.
 * Mirrors f3write/f3read. Only the free space is exercised.
 */
#define _GNU_SOURCE
#include "sdcheck/sdcheck.h"
#include "pattern.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define CHUNK (4u * 1024 * 1024)   /* I/O buffer size */

static void make_path(char *dst, size_t cap, const char *dir, int idx)
{
    snprintf(dst, cap, "%s/%d.h2w", dir, idx);
}

static void cleanup_files(const char *dir, int num_files)
{
    char path[512];
    for (int i = 1; i <= num_files; i++) {
        make_path(path, sizeof(path), dir, i);
        unlink(path);
    }
}

static int fail(sdcheck_result *r, const char *msg)
{
    r->verdict = SDCHECK_ERROR;
    snprintf(r->message, sizeof(r->message), "%s", msg);
    return -1;
}

int sdcheck_run_file(const char *mountpoint,
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

    uint64_t file_size = opt->file_size_bytes ? opt->file_size_bytes
                                              : 1024ULL * 1024 * 1024;
    uint64_t seed = opt->seed ? opt->seed : sdcheck_random_seed();

    /* Capacity context from the filesystem. */
    struct statvfs vfs;
    if (statvfs(mountpoint, &vfs) != 0)
        return fail(result, "Cannot stat mountpoint (does it exist / is it mounted?)");

    uint64_t frsize   = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    uint64_t fs_total = (uint64_t)vfs.f_blocks * frsize;
    uint64_t fs_free  = (uint64_t)vfs.f_bavail * frsize;
    uint64_t used     = fs_total - (uint64_t)vfs.f_bfree * frsize;
    result->announced_bytes = fs_total;

    if (fs_free < CHUNK)
        return fail(result, "Not enough free space on the target to run a test.");

    unsigned char *wbuf = malloc(CHUNK);
    unsigned char *rbuf = malloc(CHUNK);
    unsigned char *ebuf = malloc(CHUNK);
    if (!wbuf || !rbuf || !ebuf) {
        free(wbuf); free(rbuf); free(ebuf);
        return fail(result, "Out of memory.");
    }

    sdcheck_rate wrate, rrate;
    sdcheck_rate_init(&wrate);
    sdcheck_rate_init(&rrate);

    int rc = 0;
    int num_files = 0;
    uint64_t abs_off = 0;          /* continuous offset across all files */
    uint64_t since_sync = 0;       /* bytes written since last fdatasync */
    int disk_full = 0;
    int cancelled = 0;

    if (cb) cb(SDCHECK_PHASE_WRITE, 0, fs_free, 0.0, user_data);

    /* ---- write phase ---- */
    for (int idx = 1; !disk_full && !cancelled; idx++) {
        char path[512];
        make_path(path, sizeof(path), mountpoint, idx);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { rc = fail(result, "Cannot create test file (permission?)."); goto done; }
        num_files = idx;

        uint64_t in_file = 0;
        while (in_file < file_size) {
            if (cancel && *cancel) { cancelled = 1; break; }
            size_t want = (size_t)((file_size - in_file < CHUNK) ? (file_size - in_file) : CHUNK);
            pattern_fill(wbuf, want, seed, abs_off);

            double t0 = sdcheck_now();
            ssize_t w = write(fd, wbuf, want);
            if (w > 0) {
                /* Force a window's worth of data to the card before stopping
                   the clock, so the rate reflects media speed, not the copy
                   into the page cache. */
                since_sync += (uint64_t)w;
                if (since_sync >= SDCHECK_RATE_WINDOW) {
                    fdatasync(fd);
                    since_sync = 0;
                }
            }
            double dt = sdcheck_now() - t0;

            if (w < 0) {
                if (errno == ENOSPC) { disk_full = 1; break; }
                close(fd);
                rc = fail(result, "Write error during test.");
                goto done;
            }
            sdcheck_rate_add(&wrate, (uint64_t)w, dt);
            result->bytes_written += (uint64_t)w;
            abs_off += (uint64_t)w;
            in_file += (uint64_t)w;

            if (cb) cb(SDCHECK_PHASE_WRITE, result->bytes_written, fs_free,
                       sdcheck_rate_live(&wrate), user_data);

            if ((size_t)w < want) { disk_full = 1; break; }  /* short write = full */
        }
        fsync(fd);
        close(fd);
        if (in_file < file_size) disk_full = 1;  /* couldn't fill -> card is full */
    }

    if (cancelled) { rc = 0; snprintf(result->message, sizeof(result->message), "Cancelled."); goto done; }

    /* ---- read / verify phase ---- */
    if (cb) cb(SDCHECK_PHASE_READ, 0, result->bytes_written, 0.0, user_data);
    abs_off = 0;
    long first_bad = -1;
    for (int idx = 1; idx <= num_files && !cancelled; idx++) {
        char path[512];
        make_path(path, sizeof(path), mountpoint, idx);
        int fd = open(path, O_RDONLY);
        if (fd < 0) break;
        /* Evict the just-written pages so read-back is served by the card, not
           the page cache (otherwise read speed is meaningless memory speed). */
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        for (;;) {
            if (cancel && *cancel) { cancelled = 1; break; }
            double t0 = sdcheck_now();
            ssize_t r = read(fd, rbuf, CHUNK);
            double dt = sdcheck_now() - t0;
            if (r < 0) { close(fd); rc = fail(result, "Read error during verify."); goto done; }
            if (r == 0) break;
            sdcheck_rate_add(&rrate, (uint64_t)r, dt);

            /* Compare against the regenerated expected pattern, counting bytes. */
            pattern_fill(ebuf, (size_t)r, seed, abs_off);
            for (ssize_t i = 0; i < r; i++) {
                if (rbuf[i] != ebuf[i]) {
                    result->bad_bytes++;
                    if (first_bad < 0) first_bad = (long)(abs_off + (uint64_t)i);
                }
            }
            result->bytes_verified += (uint64_t)r;
            abs_off += (uint64_t)r;
            if (cb) cb(SDCHECK_PHASE_READ, result->bytes_verified, result->bytes_written,
                       sdcheck_rate_live(&rrate), user_data);
        }
        close(fd);
    }

    if (cancelled) { rc = 0; snprintf(result->message, sizeof(result->message), "Cancelled."); goto done; }

    /* ---- verdict ---- */
    sdcheck_rate_finish(&wrate);
    sdcheck_rate_finish(&rrate);
    result->avg_write_bps = sdcheck_rate_avg(&wrate);
    result->min_write_bps = sdcheck_rate_min(&wrate);
    result->max_write_bps = sdcheck_rate_max(&wrate);
    result->avg_read_bps  = sdcheck_rate_avg(&rrate);
    result->min_read_bps  = sdcheck_rate_min(&rrate);
    result->max_read_bps  = sdcheck_rate_max(&rrate);
    result->speed_class   = sdcheck_speed_class_from_bps(result->avg_write_bps);

    if (result->bad_bytes > 0) {
        result->verdict = SDCHECK_FAKE;
        uint64_t good_free = first_bad >= 0 ? (uint64_t)first_bad : 0;
        result->real_bytes = used + good_free;
        snprintf(result->message, sizeof(result->message),
                 "FAKE: data corrupted after ~%.2f GB; estimated real capacity ~%.2f GB.",
                 (double)good_free / 1e9, (double)result->real_bytes / 1e9);
    } else {
        result->real_bytes = used + result->bytes_written;
        if (fs_free < fs_total * 9 / 10) {
            result->verdict = SDCHECK_LIMITED_TEST;
            snprintf(result->message, sizeof(result->message),
                     "OK so far: free space (%.2f GB) verified intact, but used space was not tested.",
                     (double)result->bytes_written / 1e9);
        } else {
            result->verdict = SDCHECK_GENUINE;
            snprintf(result->message, sizeof(result->message),
                     "GENUINE: %.2f GB of free space written and verified with no errors.",
                     (double)result->bytes_written / 1e9);
        }
    }

done:
    if (cb) cb(SDCHECK_PHASE_CLEANUP, 0, 0, 0.0, user_data);
    if (!opt->keep_files)
        cleanup_files(mountpoint, num_files);
    free(wbuf); free(rbuf); free(ebuf);
    if (cb) cb(SDCHECK_PHASE_DONE, 0, 0, 0.0, user_data);
    return rc;
}
