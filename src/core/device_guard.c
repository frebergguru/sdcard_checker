// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Shared safety gate for destructive raw-device operations (test + format).
 * Refuses unless the caller confirmed, is root, and the target is a whole-disk
 * removable device — the guards that keep a fat-fingered /dev/sda from wiping
 * the system disk. If the (removable, confirmed) target or its partitions are
 * mounted, they are unmounted automatically; if a mount is busy and cannot be
 * unmounted, the operation is refused rather than writing under a live mount.
 */
#define _GNU_SOURCE
#include "sdcheck/sdcheck.h"
#include "internal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>

#define MAX_MOUNTS 32

static const char *leaf_of(const char *node)
{
    const char *s = strrchr(node, '/');
    return s ? s + 1 : node;
}

static int sysfs_removable(const char *leaf)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/block/%s/removable", leaf);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = 0;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

/*
 * Does mount source `src` belong to whole disk `devnode`? True for the disk
 * itself and its partitions (sdb -> sdb1, mmcblk0 -> mmcblk0p1), but NOT for a
 * different disk that merely shares a prefix (sdb must not match sdba).
 */
static int belongs_to_device(const char *src, const char *devnode, size_t n)
{
    if (strncmp(src, devnode, n) != 0) return 0;
    char c = src[n];
    if (c == '\0') return 1;                 /* the whole-disk node itself     */
    if (c >= '0' && c <= '9') return 1;       /* sdb1, sdb12                    */
    if (c == 'p' && src[n + 1] >= '0' && src[n + 1] <= '9')
        return 1;                             /* mmcblk0p1, nvme0n1p1           */
    return 0;
}

/* Decode /proc/mounts octal escapes (e.g. "\040" -> ' ') into a real path. */
static void unescape_path(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; ) {
        if (in[i] == '\\' &&
            in[i + 1] >= '0' && in[i + 1] <= '7' &&
            in[i + 2] >= '0' && in[i + 2] <= '7' &&
            in[i + 3] >= '0' && in[i + 3] <= '7') {
            out[o++] = (char)((in[i + 1] - '0') * 64 +
                              (in[i + 2] - '0') * 8 +
                              (in[i + 3] - '0'));
            i += 4;
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

/* Collect the (unescaped) mountpoints of devnode and its partitions. */
static int collect_mounts(const char *devnode, char mnts[][160], int max)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char src[256], tgt[160];
    size_t n = strlen(devnode);
    int cnt = 0;
    while (cnt < max && fscanf(f, "%255s %159s %*s %*s %*d %*d", src, tgt) == 2) {
        if (belongs_to_device(src, devnode, n))
            unescape_path(tgt, mnts[cnt++], 160);
    }
    fclose(f);
    return cnt;
}

static int fail(sdcheck_result *r, const char *msg)
{
    r->verdict = SDCHECK_ERROR;
    snprintf(r->message, sizeof(r->message), "%s", msg);
    return -1;
}

/*
 * Unmount every mountpoint of the device. Deepest paths first so nested mounts
 * come off in the right order. Returns 0 if everything is unmounted, -1 (with a
 * filled-in result message) if a mount is busy and cannot be released.
 */
static int unmount_all(const char *devnode, sdcheck_result *result)
{
    char mnts[MAX_MOUNTS][160];
    int nm = collect_mounts(devnode, mnts, MAX_MOUNTS);

    /* Insertion sort by descending path length (few entries, simple is fine). */
    for (int i = 1; i < nm; i++) {
        char tmp[160];
        memcpy(tmp, mnts[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strlen(mnts[j]) < strlen(tmp)) {
            memcpy(mnts[j + 1], mnts[j], sizeof(tmp));
            j--;
        }
        memcpy(mnts[j + 1], tmp, sizeof(tmp));
    }

    for (int i = 0; i < nm; i++) {
        if (umount2(mnts[i], 0) != 0 && errno != EINVAL /* already gone */) {
            int e = errno;
            result->verdict = SDCHECK_ERROR;
            snprintf(result->message, sizeof(result->message),
                     "Refusing: %.20s is mounted at %.104s and could not be "
                     "unmounted (%.36s). Close anything using it and try again.",
                     devnode, mnts[i], strerror(e));
            return -1;
        }
    }
    return 0;
}

int sdcheck_guard_destructive(const char *devnode, int confirm,
                              sdcheck_result *result)
{
    if (!confirm)
        return fail(result, "Refusing: destructive operation requires explicit confirmation.");

    /* Virtual device: a regular file (a disk image) is treated as the device,
       for testing without real hardware. It isn't a system disk and needs no
       root, so the removable/mount/root guards don't apply — only confirmation.
       This shortcut is restricted to the *unprivileged* path: we lstat() so a
       symlink is never classified as a regular file, and we require that we are
       NOT running as root. Together these stop a privileged caller (the pkexec
       helper or the D-Bus mechanism) from being steered — e.g. via a world-
       writable /dev/shm symlink — into overwriting an arbitrary file as root.
       The privileged path falls through to the removable-block-device checks
       below and only ever operates on real removable disks. */
    struct stat st;
    if (lstat(devnode, &st) == 0 && S_ISREG(st.st_mode) && geteuid() != 0)
        return 0;

    if (geteuid() != 0)
        return fail(result, "This operation requires root. Re-run with sudo.");

    int rm = sysfs_removable(leaf_of(devnode));
    if (rm < 0)
        return fail(result, "Not a whole-disk removable device (pass e.g. /dev/sdb, not a partition).");
    if (rm != 1)
        return fail(result, "Refusing: device is not removable (safety guard against system disks).");

    /* Removable and confirmed: unmount it (and its partitions) if mounted. */
    return unmount_all(devnode, result);
}
