// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Low-level format / erase of a removable device (DESTRUCTIVE, needs root).
 *
 * "Low-level format" on flash means erasing every addressable block. We do that
 * either with BLKDISCARD (the controller's TRIM/erase — fast, when supported)
 * or by overwriting the whole device with zeros, or both. An optional mkfs
 * step lays down a fresh filesystem afterward.
 *
 * Reuses the shared destructive-operation safety gate (device_guard.c).
 */
#define _GNU_SOURCE
#include "sdcheck/sdcheck.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/fs.h>

#define CHUNK (4u * 1024 * 1024)

static int fail(sdcheck_result *r, const char *msg)
{
    r->verdict = SDCHECK_ERROR;
    snprintf(r->message, sizeof(r->message), "%s", msg);
    return -1;
}

/* Only allow a small allow-list of mkfs types and well-formed names. */
static int valid_mkfs_type(const char *t)
{
    static const char *ok[] = { "vfat", "exfat", "ext4", "ext2", "ntfs", NULL };
    for (int i = 0; ok[i]; i++)
        if (strcmp(t, ok[i]) == 0) return 1;
    return 0;
}

static int try_discard(int fd, uint64_t size)
{
    uint64_t range[2] = { 0, size };
    return ioctl(fd, BLKDISCARD, &range);  /* 0 on success, -1 if unsupported */
}

static int zero_fill(int fd, uint64_t size, sdcheck_progress_cb cb,
                     void *user_data, volatile int *cancel, sdcheck_result *result)
{
    void *zeros = calloc(1, CHUNK);
    if (!zeros) return fail(result, "Out of memory.");

    sdcheck_rate wrate;
    sdcheck_rate_init(&wrate);
    lseek(fd, 0, SEEK_SET);

    if (cb) cb(SDCHECK_PHASE_WRITE, 0, size, 0.0, user_data);
    uint64_t since_sync = 0;       /* bytes written since last fdatasync */
    for (uint64_t off = 0; off < size; ) {
        if (cancel && *cancel) { free(zeros); snprintf(result->message, sizeof(result->message), "Cancelled."); result->verdict = SDCHECK_ERROR; return -1; }
        size_t want = (size_t)((size - off < CHUNK) ? (size - off) : CHUNK);
        double t0 = sdcheck_now();
        ssize_t w = write(fd, zeros, want);
        if (w > 0) {
            /* Push a window to media before stopping the clock so the erase
               rate reflects the card, not the page cache. */
            since_sync += (uint64_t)w;
            if (since_sync >= SDCHECK_RATE_WINDOW) {
                fdatasync(fd);
                since_sync = 0;
            }
        }
        double dt = sdcheck_now() - t0;
        if (w <= 0) { free(zeros); return fail(result, "Write error during erase."); }
        sdcheck_rate_add(&wrate, (uint64_t)w, dt);
        result->bytes_written += (uint64_t)w;
        off += (uint64_t)w;
        if (cb) cb(SDCHECK_PHASE_WRITE, off, size, sdcheck_rate_live(&wrate), user_data);
    }
    fsync(fd);
    free(zeros);

    sdcheck_rate_finish(&wrate);
    result->avg_write_bps = sdcheck_rate_avg(&wrate);
    result->min_write_bps = wrate.min_bps;
    result->max_write_bps = wrate.max_bps;
    result->speed_class   = sdcheck_speed_class_from_bps(result->avg_write_bps);
    return 0;
}

/* Fork/exec a program, optionally feeding it a script on stdin, with output
   silenced. Returns 0 only if it exits 0. */
static int run_quiet(char *const argv[], const char *stdin_text)
{
    int in[2] = { -1, -1 };
    if (stdin_text && pipe(in) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { if (in[0] >= 0) { close(in[0]); close(in[1]); } return -1; }
    if (pid == 0) {
        if (stdin_text) { dup2(in[0], STDIN_FILENO); close(in[0]); close(in[1]); }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (stdin_text) {
        close(in[0]);
        size_t len = strlen(stdin_text), off = 0;
        while (off < len) {
            ssize_t n = write(in[1], stdin_text + off, len - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        close(in[1]);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* MBR partition-type id for a filesystem (most compatible with cameras/Windows). */
static const char *mbr_type_for(const char *fs)
{
    if (!strcmp(fs, "exfat") || !strcmp(fs, "ntfs")) return "7";  /* HPFS/NTFS/exFAT */
    if (!strcmp(fs, "vfat"))                          return "c";  /* W95 FAT32 (LBA) */
    return "83";                                                   /* Linux           */
}

/* Name of partition <n> on <dev>: sdb -> sdb1, but mmcblk0/nvme0n1/loop0 -> ...p1. */
static void partition_node(const char *dev, int n, char *out, size_t cap)
{
    size_t len = strlen(dev);
    int last_digit = (len > 0 && isdigit((unsigned char)dev[len - 1]));
    snprintf(out, cap, "%s%s%d", dev, last_digit ? "p" : "", n);
}

/* Wait (up to ~5s) for a freshly-created partition node to appear via udev. */
static int wait_for_node(const char *node)
{
    for (int i = 0; i < 50; i++) {
        if (access(node, F_OK) == 0) return 0;
        nanosleep(&(struct timespec){ 0, 100L * 1000 * 1000 }, NULL);  /* 100 ms */
    }
    return -1;
}

/* Wipe signatures and write a single whole-disk MBR partition of the type that
   matches the filesystem we are about to create. The device must not be open
   elsewhere (the caller closes its fd first) so the kernel re-reads the table. */
static int make_one_partition(const char *devnode, const char *fstype,
                              sdcheck_result *result)
{
    char script[64];
    /* sfdisk script: DOS label, one partition, default start/size, typed. */
    snprintf(script, sizeof(script), "label: dos\n,,%s\n", mbr_type_for(fstype));

    char *const argv[] = { "sfdisk", "--wipe", "always",
                           "--quiet", (char *)devnode, NULL };
    if (run_quiet(argv, script) != 0) {
        snprintf(result->message + strlen(result->message),
                 sizeof(result->message) - strlen(result->message),
                 " (partitioning failed — is sfdisk installed?)");
        return -1;
    }
    /* Best-effort: let udev create the partition node before we mkfs it. */
    char *const settle[] = { "udevadm", "settle", NULL };
    run_quiet(settle, NULL);
    return 0;
}

static int run_mkfs(const char *devnode, const char *type, sdcheck_result *result)
{
    /* Exec mkfs.<type> directly — no shell, so the device path can never be
       interpreted as shell syntax (type is allow-listed, but exec is safer). */
    char prog[24];
    snprintf(prog, sizeof(prog), "mkfs.%s", type);

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(result->message + strlen(result->message),
                 sizeof(result->message) - strlen(result->message),
                 " (could not fork to run %s)", prog);
        return -1;
    }
    if (pid == 0) {
        /* child: silence output, then exec; "--" stops option parsing. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); }
        /* devnode is always an absolute /dev/... path from our enumeration, so
           it can't be mistaken for an option flag. */
        char *const args[] = { prog, (char *)devnode, NULL };
        execvp(prog, args);
        _exit(127);            /* exec failed (mkfs.<type> not installed) */
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(result->message + strlen(result->message),
                 sizeof(result->message) - strlen(result->message),
                 " (%s failed — is it installed?)", prog);
        return -1;
    }
    return 0;
}

int sdcheck_format_device(const char *devnode,
                          const sdcheck_format_options *opt,
                          sdcheck_progress_cb cb,
                          void *user_data,
                          volatile int *cancel,
                          sdcheck_result *result)
{
    sdcheck_format_options def;
    if (!opt) { memset(&def, 0, sizeof(def)); def.method = SDCHECK_FMT_DISCARD_THEN_ZERO; opt = &def; }
    memset(result, 0, sizeof(*result));
    result->verdict = SDCHECK_ERROR;

    /* A partition table with no filesystem is rarely useful, so a bare
       --partition defaults to exFAT (the usual choice for >32 GB cards). */
    const char *mkfs_type = opt->mkfs_type;
    if (opt->make_partition && !mkfs_type[0])
        mkfs_type = "exfat";

    if (mkfs_type[0] && !valid_mkfs_type(mkfs_type))
        return fail(result, "Unsupported filesystem type for mkfs.");

    if (sdcheck_guard_destructive(devnode, opt->confirm_destroy, result) != 0)
        return -1;

    int fd = open(devnode, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return fail(result, "Cannot open device (need root / correct path).");

    uint64_t size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) != 0 || size == 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
            size = (uint64_t)st.st_size;   /* regular-file virtual device */
        else {
            close(fd);
            return fail(result, "Cannot determine device size.");
        }
    }
    if (size > SDCHECK_MAX_SANE_BYTES) {
        close(fd);
        return fail(result,
            "Device reports an implausible capacity (over 64 TB) — most likely a "
            "flaky reader or a fake. Re-seat the card, or use a native SD slot.");
    }
    result->announced_bytes = size;

    sdcheck_fmt_method method = opt->method;
    int discarded = 0;
    if (method == SDCHECK_FMT_DISCARD || method == SDCHECK_FMT_DISCARD_THEN_ZERO) {
        if (cb) cb(SDCHECK_PHASE_WRITE, 0, size, 0.0, user_data);
        if (try_discard(fd, size) == 0) discarded = 1;
    }

    /* Zero-fill when asked, or as a fallback when discard was requested but
       the device doesn't support it. */
    int need_zero = (method == SDCHECK_FMT_ZERO) ||
                    (method == SDCHECK_FMT_DISCARD_THEN_ZERO) ||
                    (method == SDCHECK_FMT_DISCARD && !discarded);

    if (need_zero) {
        if (zero_fill(fd, size, cb, user_data, cancel, result) != 0) { close(fd); return -1; }
    }

    fsync(fd);
    close(fd);

    result->verdict = SDCHECK_GENUINE;
    snprintf(result->message, sizeof(result->message),
             "Erase complete (%s%s) over %.2f GB.",
             discarded ? "discard" : "",
             need_zero ? (discarded ? " + zero-fill" : "zero-fill") : "",
             (double)size / 1e9);

    /* Optional: lay down a partition table + filesystem so the card is ready to
       use. The device fd is already closed above, so the kernel can re-read the
       new table. With make_partition we mkfs the first partition; otherwise the
       filesystem goes straight onto the whole device (superfloppy). */
    if (mkfs_type[0]) {
        if (cb) cb(SDCHECK_PHASE_CLEANUP, 0, 0, 0.0, user_data);

        const char *target = devnode;
        char part[80];
        if (opt->make_partition) {
            if (make_one_partition(devnode, mkfs_type, result) != 0) {
                if (cb) cb(SDCHECK_PHASE_DONE, 0, 0, 0.0, user_data);
                return 0;   /* erase succeeded; report the partitioning failure */
            }
            partition_node(devnode, 1, part, sizeof(part));
            if (wait_for_node(part) != 0) {
                snprintf(result->message + strlen(result->message),
                         sizeof(result->message) - strlen(result->message),
                         " (partition %s did not appear)", part);
                if (cb) cb(SDCHECK_PHASE_DONE, 0, 0, 0.0, user_data);
                return 0;
            }
            target = part;
        }

        if (run_mkfs(target, mkfs_type, result) == 0) {
            size_t mlen = strlen(result->message);
            if (opt->make_partition)
                snprintf(result->message + mlen, sizeof(result->message) - mlen,
                         " Fresh %s partition created on %s.", mkfs_type, target);
            else
                snprintf(result->message + mlen, sizeof(result->message) - mlen,
                         " Fresh %s filesystem created.", mkfs_type);
        }
    }

    if (cb) cb(SDCHECK_PHASE_DONE, 0, 0, 0.0, user_data);
    return 0;
}
