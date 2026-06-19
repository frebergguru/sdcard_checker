/*
 * Card identity reader.
 *
 * For cards in a native SD/MMC slot, the kernel exposes the on-card CID/CSD
 * registers under /sys/block/<dev>/device/. These reveal manufacturer, product
 * name, serial and manufacture date — values that are often bogus on
 * counterfeits. Behind a USB card reader the SD CID is not exposed to the host
 * (the reader only speaks SCSI block I/O), so we fall back to the reader's
 * SCSI/USB vendor + model strings.
 */
#define _GNU_SOURCE
#include "sdcheck/sdcheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *leaf_of(const char *node)
{
    const char *s = strrchr(node, '/');
    return s ? s + 1 : node;
}

/*
 * Map a device node to the whole-disk leaf under /sys/block, where the CID /
 * SCSI-inquiry attributes actually live. A whole disk maps to itself; a
 * partition node (sdb1, mmcblk0p1, nvme0n1p1, loop0p1) maps to its parent disk.
 */
static void resolve_disk_leaf(const char *node, char *out, size_t cap)
{
    const char *leaf = leaf_of(node);
    char path[128];
    snprintf(path, sizeof(path), "/sys/block/%s", leaf);
    if (access(path, F_OK) == 0) { snprintf(out, cap, "%s", leaf); return; }

    /* Not a whole disk: strip the trailing partition number, plus a 'p'
       separator when it follows a digit (mmcblk0p1, nvme0n1p1, loop0p1). */
    size_t n = strlen(leaf);
    while (n > 0 && leaf[n - 1] >= '0' && leaf[n - 1] <= '9') n--;
    if (n >= 2 && leaf[n - 1] == 'p' && leaf[n - 2] >= '0' && leaf[n - 2] <= '9') n--;
    if (n == 0 || n >= cap) { snprintf(out, cap, "%s", leaf); return; }
    memcpy(out, leaf, n);
    out[n] = '\0';
}

/* Read a one-line sysfs attribute, trimming trailing whitespace. */
static int read_attr(const char *dir, const char *name, char *out, size_t cap)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    out[0] = '\0';
    if (!f) return 0;
    if (!fgets(out, (int)cap, f)) { fclose(f); return 0; }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        out[--n] = '\0';
    return out[0] != '\0';
}

/* Common SD/MMC manufacturer IDs (low byte of manfid). */
static const char *manufacturer_name(const char *manfid_hex)
{
    long id = strtol(manfid_hex, NULL, 16) & 0xff;
    switch (id) {
        case 0x01: return "Panasonic";
        case 0x02: return "Toshiba / Kioxia";
        case 0x03: return "SanDisk";
        case 0x1b: return "Samsung";
        case 0x1d: return "ADATA";
        case 0x27: return "Phison";
        case 0x28: return "Lexar";
        case 0x31: return "Silicon Power";
        case 0x41: return "Kingston";
        case 0x6f: return "STMicroelectronics";
        case 0x74: return "Transcend";
        case 0x76: return "Patriot";
        case 0x82: return "Sony / Gobe";
        case 0x9c: return "Angelbird / SanDisk(OEM)";
        default:   return "";
    }
}

int sdcheck_read_card_info(const char *devnode, sdcheck_card_info *info)
{
    if (!devnode || !info) return -1;
    memset(info, 0, sizeof(*info));

    char disk[64];
    resolve_disk_leaf(devnode, disk, sizeof(disk));
    char dir[256];
    snprintf(dir, sizeof(dir), "/sys/block/%s/device", disk);

    /* Native MMC/SD: the CID-derived attributes are present. */
    if (read_attr(dir, "manfid", info->manfid, sizeof(info->manfid))) {
        info->available = 1;
        snprintf(info->source, sizeof(info->source), "mmc");
        snprintf(info->manufacturer, sizeof(info->manufacturer), "%s",
                 manufacturer_name(info->manfid));
        read_attr(dir, "oemid",  info->oemid,  sizeof(info->oemid));
        read_attr(dir, "name",   info->name,   sizeof(info->name));
        read_attr(dir, "serial", info->serial, sizeof(info->serial));
        read_attr(dir, "date",   info->date,   sizeof(info->date));
        read_attr(dir, "hwrev",  info->hwrev,  sizeof(info->hwrev));
        read_attr(dir, "fwrev",  info->fwrev,  sizeof(info->fwrev));
        read_attr(dir, "cid",    info->cid,    sizeof(info->cid));
        return 0;
    }

    /* USB / SCSI reader fallback: SCSI inquiry strings. */
    if (read_attr(dir, "vendor", info->usb_vendor, sizeof(info->usb_vendor)) ||
        read_attr(dir, "model",  info->usb_model,  sizeof(info->usb_model))) {
        snprintf(info->source, sizeof(info->source), "usb");
        info->available = 0;  /* card's own identity is not exposed by the reader */
        return 0;
    }

    return 0;  /* nothing exposed */
}
