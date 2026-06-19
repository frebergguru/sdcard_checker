/*
 * sdcheck — command-line front-end.
 *
 *   sdcheck list                         list removable devices
 *   sdcheck info   <devnode>             show card identity (CID/registers)
 *   sdcheck file   <mountpoint> [opts]   safe free-space test (no root)
 *   sdcheck device <devnode>    [opts]   DESTRUCTIVE raw test (root)
 *   sdcheck auto   <devnode>    [opts]   DESTRUCTIVE test, then format if genuine
 *   sdcheck selftest                     internal sanity checks
 */
#define _GNU_SOURCE
#include "sdcheck/sdcheck.h"
#include "pattern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* A regular file used as a virtual device needs no root (it's just a file). */
static int is_regular_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static volatile int g_cancel = 0;
static void on_sigint(int sig) { (void)sig; g_cancel = 1; }

/* Full argv, captured in main(), so a root-requiring command can re-run itself. */
static char **g_argv;

/*
 * If we are not root, re-exec the exact same command under sudo so the user
 * doesn't have to prepend it themselves. On success this never returns (the
 * process image is replaced); on failure it returns and the caller proceeds,
 * letting the core's guard report the missing privileges cleanly.
 */
static void reexec_with_sudo(void)
{
    if (geteuid() == 0) return;

    char exe[4096];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return;          /* fall back to the plain "needs root" path */
    exe[len] = '\0';

    int n = 0;
    while (g_argv[n]) n++;
    char **nv = calloc((size_t)n + 2, sizeof(*nv));
    if (!nv) return;
    nv[0] = "sudo";
    nv[1] = exe;                   /* absolute path to this binary             */
    for (int i = 1; i < n; i++) nv[i + 1] = g_argv[i];   /* original arguments */
    nv[n + 1] = NULL;

    fprintf(stderr, "Root required — re-running under sudo…\n");
    execvp("sudo", nv);
    perror("sudo");                /* only reached if sudo could not be run    */
    free(nv);
}

static void fmt_gb(uint64_t b, char *out, size_t cap)
{
    snprintf(out, cap, "%.2f GB", (double)b / 1e9);
}

/* ---- progress rendering ------------------------------------------------- */

static sdcheck_phase s_last_phase = (sdcheck_phase)-1;

static void progress(sdcheck_phase phase, uint64_t done, uint64_t total,
                     double cur_bps, void *user)
{
    (void)user;
    if (phase == SDCHECK_PHASE_DONE || phase == SDCHECK_PHASE_CLEANUP) return;
    if (phase != s_last_phase) {
        if (s_last_phase != (sdcheck_phase)-1) fputc('\n', stderr);
        s_last_phase = phase;
    }
    double pct = total ? (100.0 * (double)done / (double)total) : 0.0;
    fprintf(stderr, "\r  %-8s %6.2f%%  %7.1f MB/s   ",
            sdcheck_phase_name(phase), pct, cur_bps / 1e6);
    fflush(stderr);
}

static void print_speeds(const sdcheck_result *r)
{
    if (r->avg_write_bps > 0)
        printf("  Write speed   : avg %.1f  (min %.1f / max %.1f) MB/s\n",
               r->avg_write_bps / 1e6, r->min_write_bps / 1e6, r->max_write_bps / 1e6);
    if (r->avg_read_bps > 0)
        printf("  Read speed    : avg %.1f  (min %.1f / max %.1f) MB/s\n",
               r->avg_read_bps / 1e6, r->min_read_bps / 1e6, r->max_read_bps / 1e6);
    if (r->avg_write_bps > 0)
        printf("  Speed class   : %s\n", sdcheck_speed_class_name(r->speed_class));
}

static void print_extra(const sdcheck_result *r)
{
    if (r->read_iops > 0 || r->write_iops > 0) {
        printf("  Random IOPS   : read %.0f / write %.0f (4K)\n",
               r->read_iops, r->write_iops);
        printf("  App class     : %s\n", sdcheck_app_class_name(r->app_class));
    }
    if (r->passes_total > 1)
        printf("  Passes        : %d/%d completed\n", r->passes_done, r->passes_total);
    if (r->bad_region_count > 0) {
        printf("  Bad regions   : %d%s, %.2f MB bad total\n",
               r->bad_region_count, r->bad_regions_truncated ? "+ (truncated)" : "",
               (double)r->bad_bytes / 1e6);
        int show = r->bad_region_count < 8 ? r->bad_region_count : 8;
        for (int i = 0; i < show; i++)
            printf("      [%d] @ %.2f MB  (%.2f MB)\n", i,
                   (double)r->bad_regions[i].offset / 1e6,
                   (double)r->bad_regions[i].length / 1e6);
        if (r->bad_region_count > show)
            printf("      … and %d more\n", r->bad_region_count - show);
    }
}

/* Write a JSON report; best-effort, reports its own failure to stderr. */
static void write_report(const char *path, const sdcheck_result *r, const char *device)
{
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "Could not open report file %s\n", path); return; }
    sdcheck_write_report_json(f, r, device);
    fclose(f);
    fprintf(stderr, "Report written to %s\n", path);
}

static int print_result(const sdcheck_result *r)
{
    char a[32], rb[32];
    fputc('\n', stderr);
    fmt_gb(r->announced_bytes, a, sizeof(a));
    fmt_gb(r->real_bytes, rb, sizeof(rb));
    printf("\n=== Result ===\n");
    printf("  Verdict       : %s\n", sdcheck_verdict_name(r->verdict));
    printf("  Announced     : %s\n", a);
    if (r->verdict == SDCHECK_FAKE || r->real_bytes)
        printf("  Real capacity : %s\n", rb);
    print_speeds(r);
    print_extra(r);
    printf("  %s\n", r->message);

    switch (r->verdict) {
        case SDCHECK_GENUINE:      return 0;
        case SDCHECK_LIMITED_TEST: return 0;
        case SDCHECK_FAKE:         return 2;
        default:                   return 1;
    }
}

/* ---- subcommands -------------------------------------------------------- */

static int cmd_list(void)
{
    sdcheck_device devs[32];
    int n = sdcheck_list_devices(devs, 32);
    if (n < 0) { fprintf(stderr, "Could not list devices (is lsblk available?)\n"); return 1; }
    if (n == 0) { printf("No block devices found.\n"); return 0; }
    printf("%-14s %-10s %-5s %-5s %-22s %s\n",
           "NODE", "SIZE", "RM", "TRAN", "MODEL", "MOUNTPOINT");
    for (int i = 0; i < n && i < 32; i++) {
        char sz[32]; fmt_gb(devs[i].size_bytes, sz, sizeof(sz));
        printf("%-14s %-10s %-5s %-5s %-22s %s\n",
               devs[i].node, sz, devs[i].removable ? "yes" : "no",
               devs[i].transport[0] ? devs[i].transport : "-",
               devs[i].model[0] ? devs[i].model : "-",
               devs[i].mountpoint[0] ? devs[i].mountpoint : "-");
    }
    return 0;
}

static int cmd_info(const char *node)
{
    sdcheck_card_info ci;
    if (sdcheck_read_card_info(node, &ci) != 0) { fprintf(stderr, "Bad arguments.\n"); return 1; }
    printf("=== Card identity: %s ===\n", node);
    if (ci.available) {
        printf("  Source        : %s registers\n", ci.source);
        printf("  Manufacturer  : %s%s%s\n", ci.manfid,
               ci.manufacturer[0] ? "  " : "", ci.manufacturer);
        printf("  OEM id        : %s\n", ci.oemid);
        printf("  Product name  : %s\n", ci.name);
        printf("  Serial        : %s\n", ci.serial);
        printf("  Mfg date      : %s\n", ci.date);
        if (ci.hwrev[0]) printf("  HW/FW rev     : %s / %s\n", ci.hwrev, ci.fwrev);
        printf("  CID           : %s\n", ci.cid);
        printf("\nTip: implausible manufacturer, a future date, or a name that\n"
               "     doesn't match the printed size are red flags for a fake.\n");
    } else if (ci.source[0]) {
        printf("  Card identity not exposed (USB reader or NVMe/SCSI controller).\n");
        printf("  Reader/ctrl   : %s %s\n", ci.usb_vendor, ci.usb_model);
        printf("\nFor an SD card's true CID, use a native SD slot if available.\n");
    } else {
        printf("  No identity registers exposed for this device.\n");
    }
    return 0;
}

static int cmd_file(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "Usage: sdcheck file <mountpoint> [--size GiB] [--keep]\n"); return 1; }
    const char *mount = argv[0];
    const char *report = NULL;
    sdcheck_options opt; sdcheck_options_default(&opt);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc)
            opt.file_size_bytes = strtoull(argv[++i], NULL, 10) * 1024ULL * 1024 * 1024;
        else if (!strcmp(argv[i], "--keep")) opt.keep_files = 1;
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    printf("File test on %s (safe; writes to free space only). Ctrl-C to cancel.\n", mount);
    sdcheck_result r;
    s_last_phase = (sdcheck_phase)-1;
    if (sdcheck_run_file(mount, &opt, progress, NULL, &g_cancel, &r) < 0)
        { fprintf(stderr, "\nError: %s\n", r.message); return 1; }
    if (g_cancel) { fprintf(stderr, "\nCancelled.\n"); return 130; }
    write_report(report, &r, mount);
    return print_result(&r);
}

static int cmd_device(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "Usage: sdcheck device <devnode> [--quick|--full] --yes\n");
        return 1;
    }
    const char *node = argv[0];
    const char *report = NULL;
    sdcheck_options opt; sdcheck_options_default(&opt);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes"))        opt.confirm_destroy = 1;
        else if (!strcmp(argv[i], "--full"))  opt.full_surface = 1;
        else if (!strcmp(argv[i], "--quick")) opt.full_surface = 0;
        else if (!strcmp(argv[i], "--iops"))  opt.measure_iops = 1;
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc) opt.passes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (!opt.confirm_destroy) {
        fprintf(stderr,
            "REFUSING: '%s' would be ERASED. Re-run with --yes to confirm.\n", node);
        return 1;
    }
    if (!is_regular_file(node)) reexec_with_sudo();  /* virtual-file devices need no root */
    fprintf(stderr, "DESTRUCTIVE device test on %s — all data will be lost. Ctrl-C to cancel.\n", node);
    cmd_info(node);
    sdcheck_result r;
    s_last_phase = (sdcheck_phase)-1;
    if (sdcheck_run_device(node, &opt, progress, NULL, &g_cancel, &r) < 0)
        { fprintf(stderr, "\nError: %s\n", r.message); return 1; }
    if (g_cancel) { fprintf(stderr, "\nCancelled — the device may be partially written.\n"); return 130; }
    write_report(report, &r, node);
    return print_result(&r);
}

static int cmd_format(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "Usage: sdcheck format <devnode> --yes [--discard|--zero|--both] [--mkfs TYPE]\n");
        return 1;
    }
    const char *node = argv[0];
    const char *report = NULL;
    sdcheck_format_options opt;
    memset(&opt, 0, sizeof(opt));
    opt.method = SDCHECK_FMT_DISCARD_THEN_ZERO;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes"))          opt.confirm_destroy = 1;
        else if (!strcmp(argv[i], "--discard")) opt.method = SDCHECK_FMT_DISCARD;
        else if (!strcmp(argv[i], "--zero"))    opt.method = SDCHECK_FMT_ZERO;
        else if (!strcmp(argv[i], "--both"))    opt.method = SDCHECK_FMT_DISCARD_THEN_ZERO;
        else if (!strcmp(argv[i], "--partition")) opt.make_partition = 1;
        else if (!strcmp(argv[i], "--mkfs") && i + 1 < argc)
            snprintf(opt.mkfs_type, sizeof(opt.mkfs_type), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (!opt.confirm_destroy) {
        fprintf(stderr, "REFUSING: '%s' would be ERASED. Re-run with --yes to confirm.\n", node);
        return 1;
    }
    if (!is_regular_file(node)) reexec_with_sudo();  /* virtual-file devices need no root */
    fprintf(stderr, "Low-level format of %s — all data will be erased. Ctrl-C to cancel.\n", node);
    sdcheck_result r;
    s_last_phase = (sdcheck_phase)-1;
    if (sdcheck_format_device(node, &opt, progress, NULL, &g_cancel, &r) < 0) {
        if (g_cancel) { fprintf(stderr, "\nCancelled — the device may be partially erased.\n"); return 130; }
        fprintf(stderr, "\nError: %s\n", r.message);
        return 1;
    }
    fputc('\n', stderr);
    printf("%s\n", r.message);
    if (r.avg_write_bps > 0)
        printf("Erase speed: %.1f MB/s\n", r.avg_write_bps / 1e6);
    write_report(report, &r, node);
    return 0;
}

/* Test a card and, if it passes, lay down a fresh exFAT partition — one op. */
static int cmd_auto(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr,
            "Usage: sdcheck auto <devnode> --yes [--quick|--full] [--mkfs TYPE]\n"
            "                    [--no-partition] [--discard|--zero|--both]\n");
        return 1;
    }
    const char *node = argv[0];
    const char *report = NULL;
    sdcheck_options opt; sdcheck_options_default(&opt);
    opt.full_surface = 1;                 /* auto defaults to a thorough test  */
    sdcheck_format_options fo; memset(&fo, 0, sizeof(fo));
    fo.method = SDCHECK_FMT_DISCARD;      /* the test just wrote the surface   */
    fo.make_partition = 1;                /* "creates a partition" is the point */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes"))            opt.confirm_destroy = 1;
        else if (!strcmp(argv[i], "--full"))      opt.full_surface = 1;
        else if (!strcmp(argv[i], "--quick"))     opt.full_surface = 0;
        else if (!strcmp(argv[i], "--iops"))      opt.measure_iops = 1;
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc) opt.passes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--discard"))   fo.method = SDCHECK_FMT_DISCARD;
        else if (!strcmp(argv[i], "--zero"))      fo.method = SDCHECK_FMT_ZERO;
        else if (!strcmp(argv[i], "--both"))      fo.method = SDCHECK_FMT_DISCARD_THEN_ZERO;
        else if (!strcmp(argv[i], "--no-partition")) fo.make_partition = 0;
        else if (!strcmp(argv[i], "--mkfs") && i + 1 < argc)
            snprintf(fo.mkfs_type, sizeof(fo.mkfs_type), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (!opt.confirm_destroy) {
        fprintf(stderr,
            "REFUSING: '%s' would be ERASED. Re-run with --yes to confirm.\n", node);
        return 1;
    }
    if (!is_regular_file(node)) reexec_with_sudo();  /* virtual-file devices need no root */
    fprintf(stderr, "DESTRUCTIVE auto test + format on %s — all data will be lost. Ctrl-C to cancel.\n", node);
    cmd_info(node);
    sdcheck_result r;
    s_last_phase = (sdcheck_phase)-1;
    if (sdcheck_run_auto(node, &opt, &fo, progress, NULL, &g_cancel, &r) < 0)
        { fprintf(stderr, "\nError: %s\n", r.message); return 1; }
    if (g_cancel) { fprintf(stderr, "\nCancelled — the device may be partially written.\n"); return 130; }
    write_report(report, &r, node);
    return print_result(&r);
}

/* ---- privileged helper mode -------------------------------------------- *
 * Hidden subcommand the GUI invokes via pkexec so only this small, headless
 * process runs as root (the GUI itself stays unprivileged). It speaks a
 * line-based protocol on stdout and takes cancellation on stdin:
 *
 *   P <phase> <done> <total> <bps>          progress, emitted repeatedly
 *   B <offset> <length>                      one bad region (0+ lines, before D)
 *   D <verdict> <announced> <real> <avgW> <minW> <maxW> <avgR> <minR> <maxR>
 *     <class> <readIOPS> <writeIOPS> <appClass> <passesDone> <passesTotal>
 *     <badRegionCount> <badTruncated>
 *   M <message>                              final summary line
 *
 * Exit code mirrors the normal CLI (0 ok, 2 fake, 1 error).
 * ------------------------------------------------------------------------- */

static volatile int h_cancel = 0;

/* stdin is non-blocking; the GUI only ever writes a "cancel" line. */
static void helper_poll_cancel(void)
{
    char buf[64];
    if (read(STDIN_FILENO, buf, sizeof(buf)) > 0) h_cancel = 1;
}

static void helper_progress(sdcheck_phase phase, uint64_t done, uint64_t total,
                            double bps, void *user)
{
    (void)user;
    helper_poll_cancel();
    printf("P %d %" PRIu64 " %" PRIu64 " %.3f\n", (int)phase, done, total, bps);
    fflush(stdout);
}

static void helper_emit_result(const sdcheck_result *r)
{
    for (int i = 0; i < r->bad_region_count; i++)
        printf("B %" PRIu64 " %" PRIu64 "\n",
               r->bad_regions[i].offset, r->bad_regions[i].length);
    printf("D %d %" PRIu64 " %" PRIu64 " %.3f %.3f %.3f %.3f %.3f %.3f %d "
           "%.1f %.1f %d %d %d %d %d\n",
           (int)r->verdict, r->announced_bytes, r->real_bytes,
           r->avg_write_bps, r->min_write_bps, r->max_write_bps,
           r->avg_read_bps,  r->min_read_bps,  r->max_read_bps,
           (int)r->speed_class,
           r->read_iops, r->write_iops, (int)r->app_class,
           r->passes_done, r->passes_total,
           r->bad_region_count, r->bad_regions_truncated);
    printf("M %s\n", r->message);     /* message is single-line by construction */
    fflush(stdout);
}

static int cmd_helper(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "helper: usage: helper <device|format> <node> [flags]\n"); return 1; }
    const char *op   = argv[0];
    const char *node = argv[1];

    /* Non-blocking stdin so helper_progress() can poll for cancellation. */
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl != -1) fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);

    sdcheck_result r;
    if (!strcmp(op, "device")) {
        sdcheck_options opt; sdcheck_options_default(&opt);
        opt.confirm_destroy = 1;
        for (int i = 2; i < argc; i++) {
            if      (!strcmp(argv[i], "--full"))  opt.full_surface = 1;
            else if (!strcmp(argv[i], "--quick")) opt.full_surface = 0;
            else if (!strcmp(argv[i], "--iops"))  opt.measure_iops = 1;
            else if (!strcmp(argv[i], "--passes") && i + 1 < argc) opt.passes = atoi(argv[++i]);
        }
        sdcheck_run_device(node, &opt, helper_progress, NULL, &h_cancel, &r);
    } else if (!strcmp(op, "format")) {
        sdcheck_format_options fo; memset(&fo, 0, sizeof(fo));
        fo.method = SDCHECK_FMT_DISCARD_THEN_ZERO;
        fo.confirm_destroy = 1;
        for (int i = 2; i < argc; i++) {
            if      (!strcmp(argv[i], "--discard")) fo.method = SDCHECK_FMT_DISCARD;
            else if (!strcmp(argv[i], "--zero"))    fo.method = SDCHECK_FMT_ZERO;
            else if (!strcmp(argv[i], "--both"))    fo.method = SDCHECK_FMT_DISCARD_THEN_ZERO;
            else if (!strcmp(argv[i], "--partition")) fo.make_partition = 1;
            else if (!strcmp(argv[i], "--mkfs") && i + 1 < argc)
                snprintf(fo.mkfs_type, sizeof(fo.mkfs_type), "%s", argv[++i]);
        }
        sdcheck_format_device(node, &fo, helper_progress, NULL, &h_cancel, &r);
    } else if (!strcmp(op, "auto")) {
        sdcheck_options opt; sdcheck_options_default(&opt);
        opt.confirm_destroy = 1;
        opt.full_surface = 1;
        sdcheck_format_options fo; memset(&fo, 0, sizeof(fo));
        fo.method = SDCHECK_FMT_DISCARD;
        fo.make_partition = 1;
        for (int i = 2; i < argc; i++) {
            if      (!strcmp(argv[i], "--full"))  opt.full_surface = 1;
            else if (!strcmp(argv[i], "--quick")) opt.full_surface = 0;
            else if (!strcmp(argv[i], "--iops"))  opt.measure_iops = 1;
            else if (!strcmp(argv[i], "--passes") && i + 1 < argc) opt.passes = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--discard")) fo.method = SDCHECK_FMT_DISCARD;
            else if (!strcmp(argv[i], "--zero"))    fo.method = SDCHECK_FMT_ZERO;
            else if (!strcmp(argv[i], "--both"))    fo.method = SDCHECK_FMT_DISCARD_THEN_ZERO;
            else if (!strcmp(argv[i], "--no-partition")) fo.make_partition = 0;
            else if (!strcmp(argv[i], "--mkfs") && i + 1 < argc)
                snprintf(fo.mkfs_type, sizeof(fo.mkfs_type), "%s", argv[++i]);
        }
        sdcheck_run_auto(node, &opt, &fo, helper_progress, NULL, &h_cancel, &r);
    } else {
        fprintf(stderr, "helper: unknown op '%s'\n", op);
        return 1;
    }

    helper_emit_result(&r);
    switch (r.verdict) {
        case SDCHECK_GENUINE:
        case SDCHECK_LIMITED_TEST: return 0;
        case SDCHECK_FAKE:         return 2;
        default:                   return 1;
    }
}

/* In-memory checks that don't touch any hardware. */
static int cmd_selftest(void)
{
    int fails = 0;
    unsigned char buf[4096], cmp[4096];

    /* round-trip at a few offsets */
    uint64_t offs[] = {0, 1, 7, 8, 4095, 1ULL << 30, (1ULL << 30) + 3};
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        pattern_fill(buf, sizeof(buf), 0xABCDEF, offs[i]);
        if (pattern_verify(buf, sizeof(buf), 0xABCDEF, offs[i]) != -1) {
            printf("FAIL: self-verify at offset %" PRIu64 "\n", offs[i]); fails++;
        }
    }
    /* deliberate corruption is detected at the right byte */
    pattern_fill(buf, sizeof(buf), 1, 0);
    buf[1234] ^= 0xff;
    if (pattern_verify(buf, sizeof(buf), 1, 0) != 1234) {
        printf("FAIL: corruption not located at byte 1234\n"); fails++;
    }
    /* the same content at a different offset must NOT verify (aliasing guard) */
    pattern_fill(buf, sizeof(buf), 1, 0);
    pattern_fill(cmp, sizeof(cmp), 1, 4096);
    if (memcmp(buf, cmp, sizeof(buf)) == 0) {
        printf("FAIL: pattern is offset-independent (aliasing undetectable)\n"); fails++;
    }
    /* speed-class boundaries */
    if (sdcheck_speed_class_from_bps(31e6) != SDCHECK_SPEED_U3 ||
        sdcheck_speed_class_from_bps(1e6)  != SDCHECK_SPEED_UNRATED) {
        printf("FAIL: speed-class mapping\n"); fails++;
    }

    printf("%s\n", fails ? "SELFTEST FAILED" : "SELFTEST PASSED");
    return fails ? 1 : 0;
}

static void usage(void)
{
    printf(
"sdcheck — check whether an SD/USB flash card is genuine\n\n"
"Usage:\n"
"  sdcheck list                         List removable devices\n"
"  sdcheck info   <devnode>             Show card identity (e.g. /dev/mmcblk0)\n"
"  sdcheck file   <mountpoint> [opts]   Safe test of free space (no root)\n"
"                  --size GiB             size of each test file (default 1)\n"
"                  --keep                 keep test files afterward\n"
"  sdcheck device <devnode> --yes [opts] DESTRUCTIVE raw test (needs root)\n"
"                  --quick                sparse sampling probe (default)\n"
"                  --full                 full-surface write+verify + bad-block map\n"
"                  --passes N             repeat the test N times (stress)\n"
"                  --iops                 also measure random 4K IOPS (A1/A2)\n"
"                  --report FILE          write a JSON result report\n"
"  sdcheck format <devnode> --yes [opts] DESTRUCTIVE low-level erase (root)\n"
"                  --discard|--zero|--both  erase method (default both)\n"
"                  --mkfs TYPE            make filesystem after (vfat/exfat/...)\n"
"                  --partition            add a partition table, mkfs the partition\n"
"                                           (defaults to exFAT if no --mkfs given)\n"
"                  --report FILE          write a JSON result report\n"
"  sdcheck auto   <devnode> --yes [opts] DESTRUCTIVE test, then format if genuine\n"
"                  --quick                sparse sampling probe\n"
"                  --full                 full-surface write+verify (default)\n"
"                  --passes N / --iops    as for 'device'\n"
"                  --mkfs TYPE            filesystem to create (default exfat)\n"
"                  --no-partition         mkfs the whole device (no partition table)\n"
"                  --discard|--zero|--both  erase method before mkfs (default discard)\n"
"                  --report FILE          write a JSON result report\n"
"  (file/device/format/auto all accept --report FILE)\n"
"  sdcheck selftest                     Run internal sanity checks\n\n"
"Exit codes: 0 genuine/limited, 2 fake, 1 error.\n");
}

int main(int argc, char **argv)
{
    g_argv = argv;
    signal(SIGINT, on_sigint);
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "list"))     return cmd_list();
    if (!strcmp(cmd, "selftest")) return cmd_selftest();
    if (!strcmp(cmd, "info"))     return argc < 3 ? (usage(), 1) : cmd_info(argv[2]);
    if (!strcmp(cmd, "file"))     return cmd_file(argc - 2, argv + 2);
    if (!strcmp(cmd, "device"))   return cmd_device(argc - 2, argv + 2);
    if (!strcmp(cmd, "format"))   return cmd_format(argc - 2, argv + 2);
    if (!strcmp(cmd, "auto"))     return cmd_auto(argc - 2, argv + 2);
    if (!strcmp(cmd, "helper"))   return cmd_helper(argc - 2, argv + 2);  /* GUI/pkexec */
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) { usage(); return 0; }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    usage();
    return 1;
}
