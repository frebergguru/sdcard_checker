/*
 * sdcheck_run_auto — "test, then make it usable" in one destructive op.
 *
 * Runs the raw-device capacity/authenticity test and, only if the card verifies
 * as GENUINE, lays down a fresh partition + filesystem (exFAT by default). This
 * is a thin orchestrator over sdcheck_run_device() + sdcheck_format_device(): it
 * runs entirely inside whatever privileged context already invoked it (sudo'd
 * CLI, the pkexec helper, or the D-Bus mechanism worker), so it costs a single
 * elevation rather than two.
 *
 * The returned result is the *test* result (verdict, capacities, speeds, IOPS,
 * bad-block map) with the format outcome appended to its message, so JSON
 * reports and the existing helper/D-Bus wire formats are unchanged.
 */
#define _GNU_SOURCE
#include "sdcheck/sdcheck.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* Append to result->message without overflowing its fixed 256-byte buffer. */
static void append_msg(sdcheck_result *r, const char *fmt, ...)
{
    size_t len = strlen(r->message);
    if (len >= sizeof(r->message) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->message + len, sizeof(r->message) - len, fmt, ap);
    va_end(ap);
}

int sdcheck_run_auto(const char *devnode,
                     const sdcheck_options *opt,
                     const sdcheck_format_options *fmt,
                     sdcheck_progress_cb cb, void *user_data,
                     volatile int *cancel, sdcheck_result *result)
{
    /* 1. Destructive capacity/authenticity test. Populates `result`. */
    int rc = sdcheck_run_device(devnode, opt, cb, user_data, cancel, result);
    if (rc < 0)
        return rc;                       /* test failed/errored; message is set  */
    if (cancel && *cancel)
        return 0;                        /* user aborted; leave the device as-is */

    /* 2. Only a genuine card is worth preparing for use. */
    if (result->verdict != SDCHECK_GENUINE) {
        append_msg(result, " Card not formatted (it did not pass the test).");
        return 0;
    }

    /* 3. Lay down a fresh partition + filesystem. Default to a single exFAT
       partition — the usual choice for a ready-to-use card. The format runs
       into a local result so the rich test result above is preserved. */
    sdcheck_format_options fo;
    if (fmt) {
        fo = *fmt;                         /* caller controls fs/method/partition */
    } else {
        memset(&fo, 0, sizeof(fo));
        fo.method = SDCHECK_FMT_DISCARD;   /* the test just wrote the surface */
        fo.make_partition = 1;             /* default: one partition spanning it */
    }
    fo.confirm_destroy = 1;                /* the test already confirmed intent */
    if (!fo.mkfs_type[0])
        snprintf(fo.mkfs_type, sizeof(fo.mkfs_type), "%s", "exfat");

    sdcheck_result fr;
    sdcheck_format_device(devnode, &fo, cb, user_data, cancel, &fr);

    if (fr.verdict == SDCHECK_ERROR)
        append_msg(result, " Card is genuine, but formatting failed: %s", fr.message);
    else
        append_msg(result, " %s", fr.message);

    return 0;
}
