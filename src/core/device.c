// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Removable block-device enumeration.
 *
 * Uses lsblk's machine-readable pairs output (present on Manjaro and every
 * util-linux system) rather than linking libudev, to keep dependencies
 * minimal. Disks are listed; a partition's mountpoint is attached to its
 * parent disk so the file-mode UI can offer a path to test.
 */
#define _GNU_SOURCE
#include "sdcheck/sdcheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Extract value of KEY="..." from an lsblk -P line into out (size cap). */
static int extract_pair(const char *line, const char *key, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "%s=\"", key);
    const char *p = strstr(line, pat);
    out[0] = '\0';
    if (!p) return 0;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

static sdcheck_device *find_by_node(sdcheck_device *devs, int n, const char *node)
{
    for (int i = 0; i < n; i++)
        if (strcmp(devs[i].node, node) == 0)
            return &devs[i];
    return NULL;
}

int sdcheck_list_devices(sdcheck_device *devices, int max_devices)
{
    if (!devices || max_devices <= 0) return -1;

    FILE *fp = popen(
        "lsblk -pbno NAME,TYPE,SIZE,RM,TRAN,MOUNTPOINT,PKNAME,MODEL -P 2>/dev/null",
        "r");
    if (!fp) return -1;

    int count = 0;
    char line[1024];
    char type[16], node[64], val[256], pk[64];

    while (fgets(line, sizeof(line), fp)) {
        extract_pair(line, "TYPE", type, sizeof(type));
        extract_pair(line, "NAME", node, sizeof(node));

        if (strcmp(type, "disk") == 0) {
            /* Keep counting past the buffer so the return value reflects the
               true number of disks found (the documented contract); callers cap
               their reads at max_devices, so no entry beyond the array is read. */
            if (count >= max_devices) { count++; continue; }
            sdcheck_device *d = &devices[count];
            memset(d, 0, sizeof(*d));
            snprintf(d->node, sizeof(d->node), "%s", node);

            extract_pair(line, "SIZE", val, sizeof(val));
            d->size_bytes = strtoull(val, NULL, 10);
            extract_pair(line, "RM", val, sizeof(val));
            d->removable = (val[0] == '1');
            extract_pair(line, "TRAN", d->transport, sizeof(d->transport));
            extract_pair(line, "MODEL", d->model, sizeof(d->model));
            extract_pair(line, "MOUNTPOINT", d->mountpoint, sizeof(d->mountpoint));
            count++;
        } else if (strcmp(type, "part") == 0) {
            /* Attach this partition's mountpoint to its parent disk. */
            extract_pair(line, "MOUNTPOINT", val, sizeof(val));
            if (val[0] == '\0') continue;
            extract_pair(line, "PKNAME", pk, sizeof(pk));
            sdcheck_device *parent = find_by_node(devices,
                                                  count < max_devices ? count : max_devices,
                                                  pk);
            if (parent && parent->mountpoint[0] == '\0')
                snprintf(parent->mountpoint, sizeof(parent->mountpoint), "%s", val);
        }
    }

    pclose(fp);
    return count;
}
