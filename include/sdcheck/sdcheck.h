// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * sdcheck — SD/USB flash card authenticity checker (core library)
 *
 * Detects fake-capacity flash storage by writing a deterministic pattern
 * across the claimed capacity and reading it back, and measures sustained
 * read/write throughput as a secondary signal.
 *
 * This header is the entire public API shared by the CLI and GUI front-ends.
 * The core library has no UI dependencies and never prints anything itself;
 * progress is delivered through a callback.
 */
#ifndef SDCHECK_H
#define SDCHECK_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest number of distinct bad regions recorded in a result's bad-block map. */
#define SDCHECK_MAX_BAD_REGIONS 64

/* ---- enums -------------------------------------------------------------- */

typedef enum {
    SDCHECK_PHASE_INIT = 0,
    SDCHECK_PHASE_WRITE,
    SDCHECK_PHASE_READ,
    SDCHECK_PHASE_PROBE,   /* raw-device binary search */
    SDCHECK_PHASE_IOPS,    /* random 4 KiB IOPS benchmark */
    SDCHECK_PHASE_CLEANUP,
    SDCHECK_PHASE_DONE
} sdcheck_phase;

typedef enum {
    SDCHECK_GENUINE = 0,    /* all written data verified intact            */
    SDCHECK_FAKE,           /* capacity fraud detected                     */
    SDCHECK_LIMITED_TEST,   /* only part of the card could be tested       */
    SDCHECK_ERROR           /* I/O or setup error; see message             */
} sdcheck_verdict;

/* Nominal SD speed classes, matched from measured sustained write speed. */
typedef enum {
    SDCHECK_SPEED_UNRATED = 0, /* below Class 2 (< 2 MB/s)   */
    SDCHECK_SPEED_C2,          /* >= 2  MB/s                 */
    SDCHECK_SPEED_C4,          /* >= 4  MB/s                 */
    SDCHECK_SPEED_C6,          /* >= 6  MB/s  (also V6)      */
    SDCHECK_SPEED_C10,         /* >= 10 MB/s  (also U1, V10) */
    SDCHECK_SPEED_U3,          /* >= 30 MB/s  (also V30)     */
    SDCHECK_SPEED_V60,         /* >= 60 MB/s                 */
    SDCHECK_SPEED_V90          /* >= 90 MB/s                 */
} sdcheck_speed_class;

/* SD App Performance Class, matched from measured random 4 KiB IOPS. */
typedef enum {
    SDCHECK_APP_NONE = 0,      /* below A1                                  */
    SDCHECK_APP_A1,            /* >= 1500 read & >= 500 write IOPS          */
    SDCHECK_APP_A2             /* >= 4000 read & >= 2000 write IOPS         */
} sdcheck_app_class;

/* One contiguous bad region in the bad-block map. */
typedef struct {
    uint64_t offset;
    uint64_t length;
} sdcheck_bad_region;

/* ---- options ------------------------------------------------------------ */

typedef struct {
    uint64_t file_size_bytes;  /* file-mode: size of each .h2w file (0 = 1 GiB default) */
    uint64_t seed;             /* pattern master seed (0 = fresh random seed per run)   */
    int      keep_files;       /* file-mode: 1 = do not delete .h2w files afterward     */
    int      full_surface;     /* device-mode: 1 = write+verify whole surface (slow)    */
    int      confirm_destroy;  /* device-mode: must be 1 to allow destructive writes    */
    int      passes;           /* device-mode: write+verify passes (0/1 = single)       */
    int      measure_iops;     /* device-mode: also measure random 4 KiB IOPS (A1/A2)   */
} sdcheck_options;

/* Fill an options struct with safe defaults. */
void sdcheck_options_default(sdcheck_options *opt);

/* ---- results ------------------------------------------------------------ */

typedef struct {
    sdcheck_verdict     verdict;
    uint64_t            announced_bytes;  /* capacity the card claims          */
    uint64_t            real_bytes;       /* best estimate of true usable size */
    uint64_t            bytes_written;
    uint64_t            bytes_verified;
    uint64_t            bad_bytes;        /* mismatched bytes found on read     */

    /* Throughput, in bytes/second. 0 if that phase did not run. */
    double              avg_write_bps;
    double              min_write_bps;
    double              max_write_bps;
    double              avg_read_bps;
    double              min_read_bps;
    double              max_read_bps;

    sdcheck_speed_class speed_class;      /* derived from avg_write_bps         */

    /* Random 4 KiB IOPS (App Performance Class); 0 if not measured. */
    double              read_iops;
    double              write_iops;
    sdcheck_app_class   app_class;

    /* Multi-pass stress: passes_total 0/1 = single pass. */
    int                 passes_total;
    int                 passes_done;

    /* Bad-block map (device test). bad_region_count is capped at
       SDCHECK_MAX_BAD_REGIONS; bad_regions_truncated = 1 if more were found. */
    sdcheck_bad_region  bad_regions[SDCHECK_MAX_BAD_REGIONS];
    int                 bad_region_count;
    int                 bad_regions_truncated;

    char                message[256];     /* human-readable summary / error     */
} sdcheck_result;

/* ---- progress callback -------------------------------------------------- */

/*
 * Called periodically during a test. cur_bps is the instantaneous throughput
 * of the current phase. user_data is the opaque pointer passed to the run
 * function. Return 0 to continue; the callback must be fast and must not block.
 * (Cancellation is handled via the separate cancel flag.)
 */
typedef void (*sdcheck_progress_cb)(sdcheck_phase phase,
                                    uint64_t done_bytes,
                                    uint64_t total_bytes,
                                    double cur_bps,
                                    void *user_data);

/* ---- device enumeration ------------------------------------------------- */

typedef struct {
    char     node[64];        /* e.g. /dev/sdb                          */
    char     model[128];      /* model string, may be empty             */
    char     mountpoint[256]; /* first mountpoint, may be empty         */
    char     transport[16];   /* e.g. "usb", "mmc"                      */
    uint64_t size_bytes;
    int      removable;       /* 1 if /sys reports removable            */
} sdcheck_device;

/*
 * List candidate removable devices. Caller passes an array and its capacity;
 * returns the number of devices found (may exceed max_devices, in which case
 * only max_devices were written). Returns < 0 on error.
 */
int sdcheck_list_devices(sdcheck_device *devices, int max_devices);

/* ---- card identity (read from the card's registers) --------------------- */

typedef struct {
    int  available;          /* 1 if SD/MMC registers were found            */
    char source[8];          /* "mmc", "usb/scsi", or ""                    */
    char manfid[16];         /* manufacturer id, raw (e.g. "0x000003")      */
    char manufacturer[48];   /* decoded name if the id is known             */
    char oemid[16];
    char name[16];           /* product name (e.g. "SD32G")                 */
    char serial[24];
    char date[16];           /* manufacture date, e.g. "08/2021"            */
    char hwrev[8];
    char fwrev[8];
    char cid[40];            /* raw CID register                            */
    /* USB/SCSI reader fallback (when the SD CID is masked by a reader):    */
    char usb_vendor[64];
    char usb_model[64];
} sdcheck_card_info;

/*
 * Read identity info from the card. Returns 0 on success (check
 * info->available — 0 means nothing identifiable was exposed, e.g. behind a
 * USB reader), < 0 on bad arguments.
 */
int sdcheck_read_card_info(const char *devnode, sdcheck_card_info *info);

/* ---- speed class helper ------------------------------------------------- */

sdcheck_speed_class sdcheck_speed_class_from_bps(double bytes_per_sec);
const char         *sdcheck_speed_class_name(sdcheck_speed_class c);
const char         *sdcheck_verdict_name(sdcheck_verdict v);
const char         *sdcheck_phase_name(sdcheck_phase p);

/* App Performance Class from random 4 KiB IOPS. */
sdcheck_app_class   sdcheck_app_class_from_iops(double read_iops, double write_iops);
const char         *sdcheck_app_class_name(sdcheck_app_class c);

/* Serialize a finished result to JSON. `device` is the node/path tested (may be
   NULL). Returns 0 on success. Used by both front-ends for --report / Save. */
int sdcheck_write_report_json(FILE *f, const sdcheck_result *r, const char *device);

/* ---- test entry points -------------------------------------------------- */

/*
 * File-based test: writes pattern files into an existing, writable directory
 * (a mounted card's mountpoint) until full, then reads them back. Safe: does
 * not touch existing files, no root needed. Only tests free space.
 *
 * cancel: optional pointer to a flag; set *cancel != 0 from another thread to
 *         abort. May be NULL.
 * Returns 0 on a completed run (check result->verdict), < 0 on fatal error.
 */
int sdcheck_run_file(const char *mountpoint,
                     const sdcheck_options *opt,
                     sdcheck_progress_cb cb,
                     void *user_data,
                     volatile int *cancel,
                     sdcheck_result *result);

/*
 * Raw-device test: writes directly to a block device. DESTRUCTIVE — destroys
 * all data. Requires root and opt->confirm_destroy == 1. Tests true capacity
 * via sparse sampling + binary search (and full surface if opt->full_surface).
 */
int sdcheck_run_device(const char *devnode,
                       const sdcheck_options *opt,
                       sdcheck_progress_cb cb,
                       void *user_data,
                       volatile int *cancel,
                       sdcheck_result *result);

/* ---- low-level format / erase ------------------------------------------- */

typedef enum {
    SDCHECK_FMT_DISCARD = 0,       /* TRIM/erase whole device (fast, if supported) */
    SDCHECK_FMT_ZERO,              /* overwrite whole device with zeros (thorough)  */
    SDCHECK_FMT_DISCARD_THEN_ZERO  /* discard, then zero-fill                       */
} sdcheck_fmt_method;

typedef struct {
    sdcheck_fmt_method method;
    int  confirm_destroy;          /* must be 1 to proceed                          */
    char mkfs_type[16];            /* e.g. "vfat", "exfat"; empty = no filesystem   */
    int  make_partition;           /* 1 = write a partition table + one partition   */
                                   /*     spanning the device, then mkfs on that    */
                                   /*     partition instead of the whole device     */
} sdcheck_format_options;

/*
 * Low-level erase of a removable block device (DESTRUCTIVE, needs root). Uses
 * the same safety gate as the device test. Optionally lays down a fresh
 * filesystem afterward if mkfs_type is set (shells out to mkfs.<type>).
 * On success result->verdict == SDCHECK_GENUINE and result->message describes
 * what was done; on failure SDCHECK_ERROR with a reason.
 */
int sdcheck_format_device(const char *devnode,
                          const sdcheck_format_options *opt,
                          sdcheck_progress_cb cb,
                          void *user_data,
                          volatile int *cancel,
                          sdcheck_result *result);

/*
 * Auto: run the destructive device test and, only if the card verifies as
 * GENUINE, lay down a fresh partition + filesystem (exFAT by default). One
 * destructive, root-only operation. `opt` configures the test; `fmt` configures
 * the erase/mkfs step (may be NULL for defaults — a single exFAT partition).
 * make_partition is always forced on, and an empty mkfs_type defaults to exfat.
 * The returned result is the test result with the format outcome appended to
 * its message. Returns 0 on a completed run (check result->verdict), < 0 on a
 * fatal test error.
 */
int sdcheck_run_auto(const char *devnode,
                     const sdcheck_options *opt,
                     const sdcheck_format_options *fmt,
                     sdcheck_progress_cb cb,
                     void *user_data,
                     volatile int *cancel,
                     sdcheck_result *result);

#ifdef __cplusplus
}
#endif

#endif /* SDCHECK_H */
