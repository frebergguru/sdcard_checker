// SPDX-License-Identifier: GPL-3.0-or-later
/* Small shared helpers: option defaults, speed-class mapping, name lookups. */
#include "sdcheck/sdcheck.h"

#include <inttypes.h>

#define MB (1000.0 * 1000.0)   /* SD speed classes are quoted in MB/s (decimal) */

void sdcheck_options_default(sdcheck_options *opt)
{
    if (!opt) return;
    opt->file_size_bytes = 1024ULL * 1024 * 1024; /* 1 GiB per .h2w file */
    opt->seed            = 0;  /* 0 = fresh random seed per run (unpredictable) */
    opt->keep_files      = 0;
    opt->full_surface    = 0;
    opt->confirm_destroy = 0;
    opt->passes          = 1;
    opt->measure_iops    = 0;
}

sdcheck_speed_class sdcheck_speed_class_from_bps(double bytes_per_sec)
{
    double mbps = bytes_per_sec / MB;
    if (mbps >= 90.0) return SDCHECK_SPEED_V90;
    if (mbps >= 60.0) return SDCHECK_SPEED_V60;
    if (mbps >= 30.0) return SDCHECK_SPEED_U3;
    if (mbps >= 10.0) return SDCHECK_SPEED_C10;
    if (mbps >= 6.0)  return SDCHECK_SPEED_C6;
    if (mbps >= 4.0)  return SDCHECK_SPEED_C4;
    if (mbps >= 2.0)  return SDCHECK_SPEED_C2;
    return SDCHECK_SPEED_UNRATED;
}

const char *sdcheck_speed_class_name(sdcheck_speed_class c)
{
    switch (c) {
        case SDCHECK_SPEED_V90:     return "V90 (>=90 MB/s)";
        case SDCHECK_SPEED_V60:     return "V60 (>=60 MB/s)";
        case SDCHECK_SPEED_U3:      return "U3 / V30 (>=30 MB/s)";
        case SDCHECK_SPEED_C10:     return "Class 10 / U1 / V10 (>=10 MB/s)";
        case SDCHECK_SPEED_C6:      return "Class 6 / V6 (>=6 MB/s)";
        case SDCHECK_SPEED_C4:      return "Class 4 (>=4 MB/s)";
        case SDCHECK_SPEED_C2:      return "Class 2 (>=2 MB/s)";
        case SDCHECK_SPEED_UNRATED: return "Unrated (<2 MB/s)";
    }
    return "Unknown";
}

const char *sdcheck_verdict_name(sdcheck_verdict v)
{
    switch (v) {
        case SDCHECK_GENUINE:      return "GENUINE";
        case SDCHECK_FAKE:         return "FAKE";
        case SDCHECK_LIMITED_TEST: return "LIMITED";
        case SDCHECK_ERROR:        return "ERROR";
    }
    return "UNKNOWN";
}

const char *sdcheck_phase_name(sdcheck_phase p)
{
    switch (p) {
        case SDCHECK_PHASE_INIT:    return "init";
        case SDCHECK_PHASE_WRITE:   return "writing";
        case SDCHECK_PHASE_READ:    return "reading";
        case SDCHECK_PHASE_PROBE:   return "probing";
        case SDCHECK_PHASE_IOPS:    return "iops";
        case SDCHECK_PHASE_CLEANUP: return "cleanup";
        case SDCHECK_PHASE_DONE:    return "done";
    }
    return "?";
}

/* SD spec random-4K IOPS minimums: A1 = 1500 read / 500 write, A2 = 4000/2000. */
sdcheck_app_class sdcheck_app_class_from_iops(double read_iops, double write_iops)
{
    if (read_iops >= 4000.0 && write_iops >= 2000.0) return SDCHECK_APP_A2;
    if (read_iops >= 1500.0 && write_iops >= 500.0)  return SDCHECK_APP_A1;
    return SDCHECK_APP_NONE;
}

const char *sdcheck_app_class_name(sdcheck_app_class c)
{
    switch (c) {
        case SDCHECK_APP_A2:   return "A2 (>=4000 read / 2000 write IOPS)";
        case SDCHECK_APP_A1:   return "A1 (>=1500 read / 500 write IOPS)";
        case SDCHECK_APP_NONE: return "below A1";
    }
    return "unknown";
}

/* JSON-escape a string into f (minimal: quotes, backslash, control chars). */
static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n')        fputs("\\n", f);
        else if (c == '\t')        fputs("\\t", f);
        else if (c < 0x20)         fprintf(f, "\\u%04x", c);
        else                       fputc(c, f);
    }
    fputc('"', f);
}

int sdcheck_write_report_json(FILE *f, const sdcheck_result *r, const char *device)
{
    if (!f || !r) return -1;
    fputs("{\n", f);
    fputs("  \"device\": ", f);    json_str(f, device ? device : "");        fputs(",\n", f);
    fputs("  \"verdict\": ", f);   json_str(f, sdcheck_verdict_name(r->verdict)); fputs(",\n", f);
    fprintf(f, "  \"announced_bytes\": %" PRIu64 ",\n", r->announced_bytes);
    fprintf(f, "  \"real_bytes\": %" PRIu64 ",\n", r->real_bytes);
    fprintf(f, "  \"bad_bytes\": %" PRIu64 ",\n", r->bad_bytes);
    fprintf(f, "  \"write_bps\": {\"avg\": %.0f, \"min\": %.0f, \"max\": %.0f},\n",
            r->avg_write_bps, r->min_write_bps, r->max_write_bps);
    fprintf(f, "  \"read_bps\": {\"avg\": %.0f, \"min\": %.0f, \"max\": %.0f},\n",
            r->avg_read_bps, r->min_read_bps, r->max_read_bps);
    fputs("  \"speed_class\": ", f); json_str(f, sdcheck_speed_class_name(r->speed_class)); fputs(",\n", f);
    fprintf(f, "  \"read_iops\": %.0f,\n", r->read_iops);
    fprintf(f, "  \"write_iops\": %.0f,\n", r->write_iops);
    fputs("  \"app_class\": ", f); json_str(f, sdcheck_app_class_name(r->app_class)); fputs(",\n", f);
    fprintf(f, "  \"passes\": {\"done\": %d, \"total\": %d},\n",
            r->passes_done, r->passes_total);
    fprintf(f, "  \"bad_regions_truncated\": %s,\n",
            r->bad_regions_truncated ? "true" : "false");
    fputs("  \"bad_regions\": [", f);
    for (int i = 0; i < r->bad_region_count; i++)
        fprintf(f, "%s{\"offset\": %" PRIu64 ", \"length\": %" PRIu64 "}",
                i ? ", " : "", r->bad_regions[i].offset, r->bad_regions[i].length);
    fputs("],\n", f);
    fputs("  \"message\": ", f);   json_str(f, r->message);                   fputs("\n", f);
    fputs("}\n", f);
    return 0;
}
