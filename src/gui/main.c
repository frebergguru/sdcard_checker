/*
 * sdcheck-gui — GTK4 front-end.
 *
 * Thin, *unprivileged* UI over the libsdcheck core. The safe file test runs in
 * a worker thread; the destructive device/format operations need root, so they
 * are delegated to the `sdcheck helper` subcommand launched via pkexec — only
 * that small headless process runs as root, never this GUI. Progress streams
 * back over the helper's stdout pipe (or, for the file test, via g_idle_add).
 *
 * The window is organised around the three things you can do to a card —
 * a SAFE free-space test, a DESTRUCTIVE capacity test, and a FORMAT — exposed
 * as the three pages of a GtkStack. Only the controls for the selected
 * operation are ever shown, so the destructive options are never sitting
 * greyed-out next to the safe ones.
 */
#define _GNU_SOURCE
#include "sdcheck/sdcheck.h"
#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <locale.h>

#define MAX_DEV 32

typedef struct {
    GtkWindow      *window;
    GtkDropDown    *devices;
    GtkLabel       *dev_detail;

    GtkStack       *mode_stack;     /* "file" | "device" | "format" pages     */
    GtkWidget      *mode_switcher;  /* tab bar over mode_stack                */

    GtkEntry       *path_entry;
    GtkCheckButton *confirm_device; /* arm the destructive device test        */
    GtkCheckButton *confirm_format; /* arm the destructive format             */
    GtkCheckButton *full_surface;   /* device test: write+verify whole card   */
    GtkSpinButton  *passes_spin;    /* device test: number of passes          */
    GtkCheckButton *iops_check;     /* device test: measure A1/A2 IOPS        */
    GtkDropDown    *erase_method;   /* discard / zero / both                  */
    GtkDropDown    *fs_type;        /* none / exfat / vfat / ext4 / ntfs      */
    GtkCheckButton *make_partition;

    /* auto page (test, then format if genuine) */
    GtkCheckButton *auto_full;      /* full-surface write+verify              */
    GtkSpinButton  *auto_passes;    /* stress passes                          */
    GtkCheckButton *auto_iops;      /* measure A1/A2 IOPS                     */
    GtkDropDown    *auto_fs_type;   /* exfat / vfat / ext4 / ntfs             */
    GtkCheckButton *confirm_auto;   /* arm the destructive auto op            */

    GtkButton      *info_btn;
    GtkButton      *file_btn;       /* "Start safe test"   (file page)        */
    GtkButton      *device_btn;     /* "Run device test"   (device page)      */
    GtkButton      *format_btn;     /* "Format card"       (format page)      */
    GtkButton      *auto_btn;       /* "Test & format"     (auto page)        */
    GtkButton      *cancel_btn;
    GtkProgressBar *progress;
    GtkLabel       *status;
    GtkLabel       *results;

    sdcheck_device  devs[MAX_DEV];
    int             ndev;
    volatile int    cancel;        /* file-mode worker cancel flag           */
    int             running;
    GThread        *file_thread;   /* file-mode worker, joined on shutdown   */

    /* Privileged-helper subprocess (pkexec fallback), NULL when idle. */
    GSubprocess        *proc;
    GDataInputStream   *reader;
    sdcheck_result      hres;      /* result assembled from helper D/M lines  */
    int                 hres_got;  /* 1 once the M (message) line arrived     */

    /* Tier 3 D-Bus mechanism proxy, NULL when idle / not in use. */
    GDBusProxy         *mech;
    gulong              mech_sig;
    /* Remembered request so we can fall back to pkexec if the mechanism is
       not installed on this system. */
    struct {
        char     op[8];        /* "device" | "format"                   */
        char     node[64];
        gboolean full;         /* device: full-surface write+verify     */
        int      passes;       /* device: stress passes                 */
        gboolean measure_iops; /* device: A1/A2 IOPS benchmark          */
        gboolean partition;    /* format: partition table + mkfs        */
        char     fstype[16];   /* format: mkfs type ("" = erase only)   */
        char     method[12];   /* format: "both" | "discard" | "zero"   */
    } pend;
} App;

/* ---- helpers ------------------------------------------------------------ */

static void set_results_markup(App *a, const char *markup)
{
    gtk_label_set_markup(a->results, markup);
}

/*
 * A "C" locale for parsing the helper's text protocol. GTK calls setlocale() at
 * startup, so this process may use a locale whose decimal separator is ',' —
 * but the helper subprocess prints floats with '.' (it never calls setlocale).
 * Parsing those with sscanf() under a comma locale would misread every float,
 * so we switch LC_NUMERIC to "C" around the sscanf() calls only (main thread).
 */
static locale_t c_locale(void)
{
    static locale_t loc;
    if (!loc) loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    return loc;
}

/*
 * A decoded result whose verdict is outside the known enum can't be produced
 * legitimately — it means the transfer from the privileged helper was garbled.
 * Show a clear message instead of rendering nonsense (e.g. a multi-TB
 * "capacity"). Returns TRUE if the result was rejected.
 */
static gboolean reject_bad_result(App *a)
{
    if ((int)a->hres.verdict >= SDCHECK_GENUINE &&
        (int)a->hres.verdict <= SDCHECK_ERROR)
        return FALSE;
    a->hres.verdict = SDCHECK_ERROR;
    set_results_markup(a,
        "<span color='#d11a1a' weight='bold'>Couldn't read the result.</span>\n"
        "Nothing on the card was harmed. Please try again; if it keeps happening, "
        "reinstall SD Card Checker.");
    return TRUE;
}

/* ---- shared UI updates (always called on the main thread) --------------- */

static const char *verdict_color(sdcheck_verdict v)
{
    switch (v) {
        case SDCHECK_GENUINE:      return "#2e9e44";
        case SDCHECK_LIMITED_TEST: return "#d98e04";
        case SDCHECK_FAKE:         return "#d11a1a";
        default:                   return "#888888";
    }
}

static void show_progress(App *a, sdcheck_phase phase, uint64_t done,
                          uint64_t total, double bps)
{
    double frac = total ? (double)done / (double)total : 0.0;
    if (frac > 1.0) frac = 1.0;
    gtk_progress_bar_set_fraction(a->progress, frac);
    char buf[128];
    g_snprintf(buf, sizeof(buf), "%s — %.1f%%   %.1f MB/s",
               sdcheck_phase_name(phase), frac * 100.0, bps / 1e6);
    gtk_label_set_text(a->status, buf);
}

static void render_result(App *a, const sdcheck_result *r)
{
    GString *s = g_string_new(NULL);
    g_string_append_printf(s,
        "<span size='x-large' weight='bold' color='%s'>%s</span>\n",
        verdict_color(r->verdict), sdcheck_verdict_name(r->verdict));
    g_string_append_printf(s, "<b>Announced:</b> %.2f GB\n",
                           (double)r->announced_bytes / 1e9);
    if (r->verdict == SDCHECK_FAKE || r->real_bytes)
        g_string_append_printf(s, "<b>Real capacity:</b> %.2f GB\n",
                               (double)r->real_bytes / 1e9);
    if (r->avg_write_bps > 0)
        g_string_append_printf(s, "<b>Write:</b> avg %.1f (min %.1f / max %.1f) MB/s\n",
                               r->avg_write_bps / 1e6, r->min_write_bps / 1e6, r->max_write_bps / 1e6);
    if (r->avg_read_bps > 0)
        g_string_append_printf(s, "<b>Read:</b> avg %.1f (min %.1f / max %.1f) MB/s\n",
                               r->avg_read_bps / 1e6, r->min_read_bps / 1e6, r->max_read_bps / 1e6);
    if (r->avg_write_bps > 0)
        g_string_append_printf(s, "<b>Speed class:</b> %s\n",
                               sdcheck_speed_class_name(r->speed_class));
    if (r->read_iops > 0 || r->write_iops > 0)
        g_string_append_printf(s,
            "<b>Random IOPS:</b> read %.0f / write %.0f — <b>%s</b>\n",
            r->read_iops, r->write_iops, sdcheck_app_class_name(r->app_class));
    if (r->passes_total > 1)
        g_string_append_printf(s, "<b>Passes:</b> %d/%d completed\n",
                               r->passes_done, r->passes_total);
    if (r->bad_region_count > 0) {
        uint64_t bad_total = 0;
        for (int i = 0; i < r->bad_region_count; i++)
            bad_total += r->bad_regions[i].length;
        g_string_append_printf(s,
            "<b>Bad regions:</b> %d%s (%.2f MB)\n",
            r->bad_region_count, r->bad_regions_truncated ? "+" : "",
            (double)bad_total / 1e6);
    }
    char *esc = g_markup_escape_text(r->message, -1);
    g_string_append_printf(s, "\n%s", esc);
    g_free(esc);

    set_results_markup(a, s->str);
    g_string_free(s, TRUE);
}

/* Restore the idle UI state at the end of any operation. */
static void finish_ui(App *a, gboolean error)
{
    gtk_progress_bar_set_fraction(a->progress, error ? 0.0 : 1.0);
    gtk_label_set_text(a->status, error ? "Finished — see the result below."
                                        : "Done.");
    a->running = 0;
    gtk_widget_set_sensitive(GTK_WIDGET(a->info_btn),   TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(a->file_btn),   TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(a->device_btn), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(a->format_btn), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(a->auto_btn),   TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(a->cancel_btn), FALSE);
    gtk_widget_set_sensitive(a->mode_switcher,          TRUE);
}

/* ---- file-test worker thread (safe, no root) ---------------------------- */

typedef struct { App *a; sdcheck_phase phase; uint64_t done, total; double bps; } ProgMsg;

static gboolean apply_progress(gpointer data)
{
    ProgMsg *m = data;
    show_progress(m->a, m->phase, m->done, m->total, m->bps);
    g_free(m);
    return G_SOURCE_REMOVE;
}

static void worker_progress(sdcheck_phase phase, uint64_t done, uint64_t total,
                            double bps, void *user)
{
    App *a = user;
    if (phase == SDCHECK_PHASE_CLEANUP) return;
    ProgMsg *m = g_new0(ProgMsg, 1);
    m->a = a; m->phase = phase; m->done = done; m->total = total; m->bps = bps;
    g_idle_add(apply_progress, m);
}

typedef struct { App *a; sdcheck_result r; } DoneMsg;

static gboolean apply_done(gpointer data)
{
    DoneMsg *m = data;
    App *a = m->a;
    a->hres = m->r;            /* keep for "Save report" */
    a->hres_got = 1;
    render_result(a, &a->hres);
    finish_ui(a, a->hres.verdict == SDCHECK_ERROR);
    /* The worker is finishing; drop our handle so shutdown won't try to join a
       thread that has already run to completion. */
    if (a->file_thread) { g_thread_unref(a->file_thread); a->file_thread = NULL; }
    g_free(m);
    return G_SOURCE_REMOVE;
}

typedef struct { App *a; char target[512]; } RunArgs;

static gpointer worker_thread(gpointer data)
{
    RunArgs *ra = data;
    App *a = ra->a;
    DoneMsg *dm = g_new0(DoneMsg, 1);
    dm->a = a;
    sdcheck_options opt; sdcheck_options_default(&opt);
    sdcheck_run_file(ra->target, &opt, worker_progress, a, &a->cancel, &dm->r);
    g_idle_add(apply_done, dm);
    g_free(ra);
    return NULL;
}

/* ---- device list -------------------------------------------------------- */

static void refresh_devices(App *a)
{
    a->ndev = sdcheck_list_devices(a->devs, MAX_DEV);
    if (a->ndev < 0) a->ndev = 0;
    if (a->ndev > MAX_DEV) a->ndev = MAX_DEV;

    GtkStringList *list = gtk_string_list_new(NULL);
    for (int i = 0; i < a->ndev; i++) {
        char line[384];
        g_snprintf(line, sizeof(line), "%s  •  %.1f GB  •  %s%s",
                   a->devs[i].node, (double)a->devs[i].size_bytes / 1e9,
                   a->devs[i].transport[0] ? a->devs[i].transport : "?",
                   a->devs[i].removable ? ", removable" : "");
        gtk_string_list_append(list, line);
    }
    if (a->ndev == 0) gtk_string_list_append(list, "(no removable devices found)");
    gtk_drop_down_set_model(a->devices, G_LIST_MODEL(list));
    g_object_unref(list);
}

static void update_detail(App *a)
{
    guint idx = gtk_drop_down_get_selected(a->devices);
    if ((int)idx >= a->ndev) {
        gtk_label_set_markup(a->dev_detail,
            "<i>Insert a card and press Refresh, then pick it above.</i>");
        return;
    }
    sdcheck_device *d = &a->devs[idx];

    sdcheck_card_info ci;
    sdcheck_read_card_info(d->node, &ci);

    GString *s = g_string_new(NULL);
    char *model = g_markup_escape_text(d->model[0] ? d->model : "-", -1);
    g_string_append_printf(s, "<b>Model:</b> %s    <b>Mount:</b> %s",
                           model,
                           d->mountpoint[0] ? d->mountpoint : "(not mounted)");
    g_free(model);
    if (ci.available) {
        char *name = g_markup_escape_text(ci.name, -1);
        g_string_append_printf(s, "\n<b>Card:</b> %s %s  •  serial %s  •  mfg %s",
                               ci.manufacturer[0] ? ci.manufacturer : ci.manfid,
                               name, ci.serial, ci.date);
        g_free(name);
    } else if (ci.source[0]) {
        char *vend = g_markup_escape_text(ci.usb_vendor, -1);
        char *mod  = g_markup_escape_text(ci.usb_model, -1);
        g_string_append_printf(s, "\n<b>Reader:</b> %s %s "
                               "<i>(USB readers don't reveal the card's identity)</i>",
                               vend, mod);
        g_free(vend); g_free(mod);
    }
    gtk_label_set_markup(a->dev_detail, s->str);
    g_string_free(s, TRUE);

    if (d->mountpoint[0])
        gtk_editable_set_text(GTK_EDITABLE(a->path_entry), d->mountpoint);
}

/* ---- signal handlers ---------------------------------------------------- */

static void on_refresh(GtkButton *b, gpointer u) { (void)b; App *a = u; refresh_devices(a); update_detail(a); }
static void on_dev_changed(GObject *o, GParamSpec *p, gpointer u) { (void)o; (void)p; update_detail(u); }
static void on_cancel(GtkButton *b, gpointer u)
{
    (void)b;
    App *a = u;
    a->cancel = 1;                       /* file-mode worker */
    if (a->proc) {                       /* pkexec helper: ask it to stop */
        GOutputStream *in = g_subprocess_get_stdin_pipe(a->proc);
        if (in) g_output_stream_write_all(in, "cancel\n", 7, NULL, NULL, NULL);
    }
    if (a->mech)                         /* D-Bus mechanism: Cancel() */
        g_dbus_proxy_call(a->mech, "Cancel", NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          NULL, NULL, NULL);
    gtk_label_set_text(a->status, "Cancelling…");
}

/* Copy the current result panel (plain text, no markup) to the clipboard. */
static void on_copy(GtkButton *b, gpointer u)
{
    (void)b;
    App *a = u;
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(a->results));
    gdk_clipboard_set(cb, G_TYPE_STRING, gtk_label_get_text(a->results));
    gtk_label_set_text(a->status, "Result copied to clipboard.");
}

/* Save the last result as a JSON report (GtkFileDialog, GTK >= 4.10). */
static void on_save_done(GObject *src, GAsyncResult *res, gpointer u)
{
    App *a = u;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file) return;                       /* cancelled */
    char *path = g_file_get_path(file);
    if (path) {
        FILE *f = fopen(path, "w");
        if (f) {
            sdcheck_write_report_json(f, &a->hres,
                                      a->pend.node[0] ? a->pend.node : NULL);
            fclose(f);
            gtk_label_set_text(a->status, "Report saved.");
        } else {
            gtk_label_set_text(a->status, "Could not write the report file.");
        }
        g_free(path);
    }
    g_object_unref(file);
}

static void on_save_report(GtkButton *b, gpointer u)
{
    (void)b;
    App *a = u;
    if (!a->hres_got) {
        gtk_label_set_text(a->status, "Run a test first — no result to save.");
        return;
    }
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_initial_name(dlg, "sdcheck-report.json");
    gtk_file_dialog_save(dlg, a->window, NULL, on_save_done, a);
    g_object_unref(dlg);
}

/* Read-only: dump the selected device's identity into the Result panel.
   Non-destructive — no writes, no confirmation, no root needed. */
static void on_info(GtkButton *b, gpointer u)
{
    (void)b;
    App *a = u;
    guint idx = gtk_drop_down_get_selected(a->devices);
    if ((int)idx >= a->ndev) {
        set_results_markup(a, "<i>Select a device first.</i>");
        return;
    }
    sdcheck_device *d = &a->devs[idx];

    sdcheck_card_info ci;
    sdcheck_read_card_info(d->node, &ci);

    GString *s = g_string_new(NULL);
    g_string_append(s, "<span size='large' weight='bold'>Device information</span>\n");
    g_string_append_printf(s, "<b>Node:</b> %s\n", d->node);
    g_string_append_printf(s, "<b>Capacity:</b> %.2f GB (%llu bytes)\n",
                           (double)d->size_bytes / 1e9,
                           (unsigned long long)d->size_bytes);
    g_string_append_printf(s, "<b>Transport:</b> %s%s\n",
                           d->transport[0] ? d->transport : "?",
                           d->removable ? ", removable" : "");
    g_string_append_printf(s, "<b>Model:</b> %s\n", d->model[0] ? d->model : "-");
    g_string_append_printf(s, "<b>Mount:</b> %s\n",
                           d->mountpoint[0] ? d->mountpoint : "(not mounted)");

    if (ci.available) {
        char *name   = g_markup_escape_text(ci.name[0] ? ci.name : "-", -1);
        char *serial = g_markup_escape_text(ci.serial[0] ? ci.serial : "-", -1);
        g_string_append(s, "\n<span weight='bold'>Card identity</span>\n");
        g_string_append_printf(s, "<b>Manufacturer:</b> %s (%s)\n",
                               ci.manufacturer[0] ? ci.manufacturer : "unknown",
                               ci.manfid[0] ? ci.manfid : "?");
        if (ci.oemid[0]) g_string_append_printf(s, "<b>OEM ID:</b> %s\n", ci.oemid);
        g_string_append_printf(s, "<b>Product:</b> %s\n", name);
        g_string_append_printf(s, "<b>Serial:</b> %s\n", serial);
        if (ci.date[0])  g_string_append_printf(s, "<b>Manufactured:</b> %s\n", ci.date);
        if (ci.hwrev[0] || ci.fwrev[0])
            g_string_append_printf(s, "<b>Revision:</b> hw %s / fw %s\n",
                                   ci.hwrev[0] ? ci.hwrev : "-", ci.fwrev[0] ? ci.fwrev : "-");
        if (ci.cid[0]) g_string_append_printf(s, "<b>Raw CID:</b> <tt>%s</tt>\n", ci.cid);
        g_free(name);
        g_free(serial);
    } else if (ci.source[0]) {
        char *vend = g_markup_escape_text(ci.usb_vendor, -1);
        char *mod  = g_markup_escape_text(ci.usb_model, -1);
        g_string_append(s, "\n<span weight='bold'>Reader</span>\n");
        g_string_append_printf(s, "<b>Hardware:</b> %s %s\n", vend, mod);
        g_string_append(s, "<i>USB card readers don't reveal the card's identity — "
                           "plug the card into a built-in SD slot to see it.</i>\n");
        g_free(vend);
        g_free(mod);
    } else {
        g_string_append(s, "\n<i>This device doesn't reveal the card's identity.</i>\n");
    }

    set_results_markup(a, s->str);
    g_string_free(s, TRUE);
    gtk_label_set_text(a->status, "Showing device info.");
}

/* Put the UI into the busy state shared by both the file worker and the
   privileged helper. */
static void begin_ui(App *a)
{
    a->cancel = 0;
    a->running = 1;
    gtk_widget_set_sensitive(GTK_WIDGET(a->info_btn),   FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(a->file_btn),   FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(a->device_btn), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(a->format_btn), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(a->auto_btn),   FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(a->cancel_btn), TRUE);
    gtk_widget_set_sensitive(a->mode_switcher,          FALSE);
    gtk_progress_bar_set_fraction(a->progress, 0.0);
    set_results_markup(a, "<i>Running…</i>");
    gtk_label_set_text(a->status, "Starting…");
}

static void launch_worker(App *a, RunArgs *ra)
{
    begin_ui(a);
    /* Keep the handle so on_shutdown can join the worker before App is freed,
       avoiding a use-after-free if the window is closed mid-test. */
    a->file_thread = g_thread_new("sdcheck-worker", worker_thread, ra);
}

/* Validate the selection + erase confirmation; returns the device node (owned by
   the App's device list) or NULL after showing why. */
static const char *selected_destructive_node(App *a, GtkCheckButton *confirm)
{
    guint idx = gtk_drop_down_get_selected(a->devices);
    if ((int)idx >= a->ndev) {
        gtk_label_set_text(a->status, "Select a device first.");
        return NULL;
    }
    if (!gtk_check_button_get_active(confirm)) {
        set_results_markup(a,
            "<span color='#d11a1a' weight='bold'>Tick the erase confirmation "
            "to run a destructive operation.</span>");
        return NULL;
    }
    return a->devs[idx].node;
}

/* Absolute path to the sdcheck CLI binary (the privileged helper), assumed to
   sit next to this GUI binary. */
static const char *helper_path(void)
{
    static char path[4096];
    if (path[0]) return path;
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { g_strlcpy(path, "sdcheck", sizeof(path)); return path; }
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (slash) { *slash = '\0'; g_snprintf(path, sizeof(path), "%s/sdcheck", exe); }
    else         g_strlcpy(path, "sdcheck", sizeof(path));
    return path;
}

/*
 * Stream the helper's stdout, one line at a time, on the GTK main loop. P lines
 * drive the progress bar, D/M assemble the result, and EOF (the helper or pkexec
 * closed stdout) decides the verdict — including the wrong-password / cancelled
 * case, where pkexec exits 126/127 and nothing on the card was touched.
 */
static void on_helper_line(GObject *src, GAsyncResult *res, gpointer u)
{
    App *a = u;
    GDataInputStream *in = G_DATA_INPUT_STREAM(src);
    char *line = g_data_input_stream_read_line_finish(in, res, NULL, NULL);

    if (!line) {
        g_subprocess_wait(a->proc, NULL, NULL);   /* already at EOF: returns now */
        int code = g_subprocess_get_if_exited(a->proc)
                 ? g_subprocess_get_exit_status(a->proc) : -1;
        gboolean failed = FALSE;
        if (code == 126 || code == 127) {
            set_results_markup(a,
                "<span color='#d11a1a' weight='bold'>The card was NOT changed.</span>\n"
                "The permission prompt was cancelled, or the password was wrong. "
                "Nothing on the card was read, written, or removed. Make sure the "
                "confirmation is ticked, then try again.");
            failed = TRUE;
        } else if (!a->hres_got) {
            set_results_markup(a,
                "<span color='#d11a1a' weight='bold'>The card test stopped before "
                "it finished.</span>\nNothing on the card was harmed. Please try "
                "again.");
            failed = TRUE;
        }
        finish_ui(a, failed || a->hres.verdict == SDCHECK_ERROR);
        g_clear_object(&a->reader);
        g_clear_object(&a->proc);
        return;
    }

    switch (line[0]) {
        case 'P': {
            int ph; guint64 done, total; double bps;
            locale_t old = uselocale(c_locale());
            int got = sscanf(line, "P %d %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %lf",
                             &ph, &done, &total, &bps);
            uselocale(old);
            if (got == 4)
                show_progress(a, (sdcheck_phase)ph, done, total, bps);
            break;
        }
        case 'B': {
            guint64 boff, blen;
            if (sscanf(line, "B %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
                       &boff, &blen) == 2 &&
                a->hres.bad_region_count < SDCHECK_MAX_BAD_REGIONS) {
                a->hres.bad_regions[a->hres.bad_region_count].offset = boff;
                a->hres.bad_regions[a->hres.bad_region_count].length = blen;
                a->hres.bad_region_count++;
            }
            break;
        }
        case 'D': {
            int v, cls, appcls, pdone, ptot, brc, btrunc;
            guint64 ann, real_; double aw, mw, xw, ar, mr, xr, riops, wiops;
            locale_t old = uselocale(c_locale());
            int got = sscanf(line, "D %d %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
                       " %lf %lf %lf %lf %lf %lf %d %lf %lf %d %d %d %d %d",
                       &v, &ann, &real_, &aw, &mw, &xw, &ar, &mr, &xr, &cls,
                       &riops, &wiops, &appcls, &pdone, &ptot, &brc, &btrunc);
            uselocale(old);
            if (got == 17) {
                sdcheck_result *r = &a->hres;
                r->verdict = (sdcheck_verdict)v;
                r->announced_bytes = ann; r->real_bytes = real_;
                r->avg_write_bps = aw; r->min_write_bps = mw; r->max_write_bps = xw;
                r->avg_read_bps  = ar; r->min_read_bps  = mr; r->max_read_bps  = xr;
                r->speed_class = (sdcheck_speed_class)cls;
                r->read_iops = riops; r->write_iops = wiops;
                r->app_class = (sdcheck_app_class)appcls;
                r->passes_done = pdone; r->passes_total = ptot;
                r->bad_regions_truncated = btrunc;   /* count comes from B lines */
            }
            break;
        }
        case 'M':
            g_strlcpy(a->hres.message, (line[1] == ' ') ? line + 2 : line + 1,
                      sizeof(a->hres.message));
            a->hres_got = 1;
            if (!reject_bad_result(a))   /* EOF finalizes the error state */
                render_result(a, &a->hres);
            break;
    }
    g_free(line);
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL,
                                        on_helper_line, a);
}

/* Launch the privileged helper via pkexec for a destructive op (the fallback
   when the Tier 3 D-Bus mechanism is not installed). The GUI stays unprivileged;
   only the headless helper runs as root, so there is no display for a compositor
   to refuse. */
static void start_via_pkexec(App *a)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup("pkexec"));
    g_ptr_array_add(argv, g_strdup(helper_path()));
    g_ptr_array_add(argv, g_strdup("helper"));
    g_ptr_array_add(argv, g_strdup(a->pend.op));
    g_ptr_array_add(argv, g_strdup(a->pend.node));
    if (!g_strcmp0(a->pend.op, "device")) {
        if (a->pend.full) g_ptr_array_add(argv, g_strdup("--full"));
        if (a->pend.measure_iops) g_ptr_array_add(argv, g_strdup("--iops"));
        if (a->pend.passes > 1) {
            g_ptr_array_add(argv, g_strdup("--passes"));
            g_ptr_array_add(argv, g_strdup_printf("%d", a->pend.passes));
        }
    } else if (!g_strcmp0(a->pend.op, "auto")) {
        if (a->pend.full) g_ptr_array_add(argv, g_strdup("--full"));
        else              g_ptr_array_add(argv, g_strdup("--quick"));
        if (a->pend.measure_iops) g_ptr_array_add(argv, g_strdup("--iops"));
        if (a->pend.passes > 1) {
            g_ptr_array_add(argv, g_strdup("--passes"));
            g_ptr_array_add(argv, g_strdup_printf("%d", a->pend.passes));
        }
        if (a->pend.fstype[0]) {
            g_ptr_array_add(argv, g_strdup("--mkfs"));
            g_ptr_array_add(argv, g_strdup(a->pend.fstype));
        }
        g_ptr_array_add(argv, g_strdup_printf("--%s",
            a->pend.method[0] ? a->pend.method : "discard"));
    } else {                                   /* format */
        g_ptr_array_add(argv, g_strdup_printf("--%s",
            a->pend.method[0] ? a->pend.method : "both"));
        if (a->pend.fstype[0]) {
            g_ptr_array_add(argv, g_strdup("--mkfs"));
            g_ptr_array_add(argv, g_strdup(a->pend.fstype));
        }
        if (a->pend.partition) g_ptr_array_add(argv, g_strdup("--partition"));
    }
    g_ptr_array_add(argv, NULL);

    GError *err = NULL;
    GSubprocess *proc = g_subprocess_newv((const gchar * const *)argv->pdata,
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err);
    g_ptr_array_free(argv, TRUE);

    if (!proc) {
        set_results_markup(a,
            "<span color='#d11a1a' weight='bold'>Couldn't ask for permission.</span>\n"
            "This action needs administrator approval, but the password prompt "
            "couldn't be opened. Reinstalling SD Card Checker usually fixes this.");
        if (err) g_clear_error(&err);
        return;
    }

    a->proc = proc;
    a->reader = g_data_input_stream_new(g_subprocess_get_stdout_pipe(proc));
    memset(&a->hres, 0, sizeof(a->hres));
    a->hres_got = 0;

    begin_ui(a);
    gtk_label_set_text(a->status, "Waiting for permission — enter your password…");
    g_data_input_stream_read_line_async(a->reader, G_PRIORITY_DEFAULT, NULL,
                                        on_helper_line, a);
}

/* Drop the mechanism proxy and its signal subscription. */
static void mech_drop(App *a)
{
    if (a->mech) {
        if (a->mech_sig) g_signal_handler_disconnect(a->mech, a->mech_sig);
        a->mech_sig = 0;
        g_clear_object(&a->mech);
    }
}

/* Progress/Finished signals broadcast by the D-Bus mechanism. */
static void on_mech_signal(GDBusProxy *proxy, char *sender, char *signal,
                           GVariant *params, gpointer u)
{
    (void)proxy; (void)sender;
    App *a = u;
    if (!g_strcmp0(signal, "Progress")) {
        gint ph; guint64 done, total; double bps;
        g_variant_get(params, "(ittd)", &ph, &done, &total, &bps);
        show_progress(a, (sdcheck_phase)ph, done, total, bps);
    } else if (!g_strcmp0(signal, "Finished")) {
        sdcheck_result *r = &a->hres;          /* keep it for "Save report" */
        memset(r, 0, sizeof(*r));

        /*
         * Guard against a version-skewed mechanism. The root D-Bus service is a
         * separate binary (libexec/sdcheck/sdcheck-mechanism) that rebuilding or
         * updating the GUI does NOT replace. If it was built from a different
         * source, its Finished signal carries a different field layout — and a
         * mismatched g_variant_get() fails with a GLib-CRITICAL, leaving the
         * output variables holding uninitialized stack data. That used to read
         * back as an out-of-range verdict and surface the generic, misleading
         * "result was garbled, reinstall" message. Detect the mismatch up front
         * and name the component that's actually stale.
         */
        if (!g_variant_is_of_type(params,
                G_VARIANT_TYPE("(ittddddddiddiiia(tt)s)"))) {
            set_results_markup(a,
                "<span color='#d11a1a' weight='bold'>SD Card Checker needs to be "
                "reinstalled.</span>\n"
                "Nothing on the card was harmed, but part of the app is out of "
                "date and couldn't report the result. Please reinstall SD Card "
                "Checker.");
            finish_ui(a, TRUE);
            mech_drop(a);
            return;
        }

        gint v = SDCHECK_ERROR, cls = 0, appcls = 0, pdone = 0, ptot = 0;
        guint64 ann = 0, real_ = 0;
        GVariantIter *regions = NULL;
        const char *msg = NULL;
        g_variant_get(params, "(ittddddddiddiiia(tt)&s)",
                      &v, &ann, &real_,
                      &r->avg_write_bps, &r->min_write_bps, &r->max_write_bps,
                      &r->avg_read_bps,  &r->min_read_bps,  &r->max_read_bps,
                      &cls, &r->read_iops, &r->write_iops, &appcls,
                      &pdone, &ptot, &regions, &msg);
        r->verdict = (sdcheck_verdict)v;
        r->announced_bytes = ann; r->real_bytes = real_;
        r->speed_class = (sdcheck_speed_class)cls;
        r->app_class = (sdcheck_app_class)appcls;
        r->passes_done = pdone; r->passes_total = ptot;
        guint64 boff, blen;
        while (regions && g_variant_iter_loop(regions, "(tt)", &boff, &blen)) {
            if (r->bad_region_count < SDCHECK_MAX_BAD_REGIONS) {
                r->bad_regions[r->bad_region_count].offset = boff;
                r->bad_regions[r->bad_region_count].length = blen;
                r->bad_region_count++;
            } else {
                r->bad_regions_truncated = 1;
            }
        }
        if (regions) g_variant_iter_free(regions);
        g_strlcpy(r->message, msg ? msg : "", sizeof(r->message));
        a->hres_got = 1;
        if (reject_bad_result(a)) { finish_ui(a, TRUE); mech_drop(a); return; }
        render_result(a, r);
        finish_ui(a, r->verdict == SDCHECK_ERROR);
        mech_drop(a);
    }
}

/* Reply to our Test/Format call. The op runs via signals; here we only handle
   the failure modes: not authorized, or the service simply isn't installed (in
   which case we transparently fall back to the pkexec helper). */
static void on_mech_call_done(GObject *src, GAsyncResult *res, gpointer u)
{
    App *a = u;
    GError *err = NULL;
    GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    if (r) { g_variant_unref(r); return; }   /* started; signals drive the UI */

    gboolean unavailable =
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND);
    mech_drop(a);

    if (unavailable) {
        g_clear_error(&err);
        start_via_pkexec(a);          /* Tier 1 fallback, uses a->pend */
        return;
    }
    set_results_markup(a,
        "<span color='#d11a1a' weight='bold'>The card was NOT changed.</span>\n"
        "The permission request was cancelled or denied.");
    finish_ui(a, TRUE);
    g_clear_error(&err);
}

/* Try the Tier 3 D-Bus mechanism; returns FALSE if we couldn't even create the
   proxy (no system bus), so the caller can fall back to pkexec. Uses a->pend. */
static gboolean start_via_mechanism(App *a)
{
    GError *err = NULL;
    /* The interface exposes no properties, and the bus policy only allows the
       Mechanism1 + Introspectable interfaces — so suppress the automatic
       org.freedesktop.DBus.Properties.GetAll call (it would be denied and only
       add log noise). Auto-start stays on so the service D-Bus activates. */
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NULL,
        "org.sdcheck.Mechanism", "/org/sdcheck/Mechanism",
        "org.sdcheck.Mechanism1", NULL, &err);
    if (!proxy) { g_clear_error(&err); return FALSE; }

    a->mech = proxy;
    a->mech_sig = g_signal_connect(proxy, "g-signal",
                                   G_CALLBACK(on_mech_signal), a);

    begin_ui(a);
    gtk_label_set_text(a->status, "Asking for permission…");

    const char *method;
    GVariant *args;
    if (!g_strcmp0(a->pend.op, "device")) {
        method = "Test";
        args = g_variant_new("(sbib)", a->pend.node, a->pend.full,
                             a->pend.passes > 0 ? a->pend.passes : 1,
                             a->pend.measure_iops);
    } else if (!g_strcmp0(a->pend.op, "auto")) {
        method = "Auto";
        args = g_variant_new("(sbibss)", a->pend.node, a->pend.full,
                             a->pend.passes > 0 ? a->pend.passes : 1,
                             a->pend.measure_iops,
                             a->pend.fstype,
                             a->pend.method[0] ? a->pend.method : "discard");
    } else {
        method = "Format";
        args = g_variant_new("(sssb)", a->pend.node,
                             a->pend.method[0] ? a->pend.method : "both",
                             a->pend.fstype, a->pend.partition);
    }

    g_dbus_proxy_call(proxy, method, args,
                      G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1,
                      NULL, on_mech_call_done, a);
    return TRUE;
}

/* Run the destructive op described in a->pend: prefer the D-Bus mechanism, fall
   back to pkexec. */
static void start_destructive(App *a)
{
    if (!start_via_mechanism(a))
        start_via_pkexec(a);
}

/* Dropdown index → wire values (must match the GtkStringList order built below). */
static const char *erase_method_id(guint i)
{
    static const char *m[] = { "both", "discard", "zero" };
    return (i < G_N_ELEMENTS(m)) ? m[i] : "both";
}
static const char *fs_type_id(guint i)
{
    static const char *f[] = { "", "exfat", "vfat", "ext4", "ntfs" };
    return (i < G_N_ELEMENTS(f)) ? f[i] : "";
}

/* Safe free-space test (file page). No root, keeps existing data. */
static void on_start_file(GtkButton *b, gpointer u)
{
    (void)b;
    App *a = u;
    if (a->running) return;

    const char *path = gtk_editable_get_text(GTK_EDITABLE(a->path_entry));
    if (!path || !*path) {
        gtk_label_set_text(a->status, "Enter a folder/mountpoint to test.");
        return;
    }
    g_strlcpy(a->pend.node, path, sizeof(a->pend.node));  /* for Save report */
    RunArgs *ra = g_new0(RunArgs, 1);
    ra->a = a;
    g_strlcpy(ra->target, path, sizeof(ra->target));
    launch_worker(a, ra);
}

/* Destructive raw capacity test (device page). */
static void on_device(GtkButton *b, gpointer u)
{
    (void)b;
    App *a = u;
    if (a->running) return;

    const char *node = selected_destructive_node(a, a->confirm_device);
    if (!node) return;
    memset(&a->pend, 0, sizeof(a->pend));
    g_strlcpy(a->pend.op, "device", sizeof(a->pend.op));
    g_strlcpy(a->pend.node, node, sizeof(a->pend.node));
    a->pend.full = gtk_check_button_get_active(a->full_surface);
    a->pend.measure_iops = gtk_check_button_get_active(a->iops_check);
    a->pend.passes = gtk_spin_button_get_value_as_int(a->passes_spin);
    start_destructive(a);
}

/* Low-level format (format page). */
static void on_format(GtkButton *b, gpointer u)
{
    (void)b;
    App *a = u;
    if (a->running) return;
    const char *node = selected_destructive_node(a, a->confirm_format);
    if (!node) return;

    const char *fs = fs_type_id(gtk_drop_down_get_selected(a->fs_type));
    gboolean part = gtk_check_button_get_active(a->make_partition);

    memset(&a->pend, 0, sizeof(a->pend));
    g_strlcpy(a->pend.op, "format", sizeof(a->pend.op));
    g_strlcpy(a->pend.node, node, sizeof(a->pend.node));
    g_strlcpy(a->pend.method,
              erase_method_id(gtk_drop_down_get_selected(a->erase_method)),
              sizeof(a->pend.method));
    g_strlcpy(a->pend.fstype, fs, sizeof(a->pend.fstype));
    /* A partition with no filesystem is pointless; pair the checkbox with a fs. */
    a->pend.partition = (part && fs[0]);
    start_destructive(a);
}

/* Auto: destructive test, then a fresh exFAT partition if the card is genuine. */
static void on_auto(GtkButton *b, gpointer u)
{
    (void)b;
    App *a = u;
    if (a->running) return;
    const char *node = selected_destructive_node(a, a->confirm_auto);
    if (!node) return;

    /* The auto filesystem dropdown never offers "none" — a partition is the
       whole point — so its index maps to fs_type_id() + 1. */
    const char *fs = fs_type_id(gtk_drop_down_get_selected(a->auto_fs_type) + 1);

    memset(&a->pend, 0, sizeof(a->pend));
    g_strlcpy(a->pend.op, "auto", sizeof(a->pend.op));
    g_strlcpy(a->pend.node, node, sizeof(a->pend.node));
    a->pend.full = gtk_check_button_get_active(a->auto_full);
    a->pend.measure_iops = gtk_check_button_get_active(a->auto_iops);
    a->pend.passes = gtk_spin_button_get_value_as_int(a->auto_passes);
    g_strlcpy(a->pend.fstype, fs, sizeof(a->pend.fstype));
    a->pend.partition = TRUE;
    g_strlcpy(a->pend.method, "discard", sizeof(a->pend.method));
    start_destructive(a);
}

/* ---- UI construction ---------------------------------------------------- */

static GtkWidget *labeled_row(const char *text, GtkWidget *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_set_size_request(l, 110, -1);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_box_append(GTK_BOX(box), l);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_box_append(GTK_BOX(box), w);
    return box;
}

/* A wrapped, dimmed explanatory line used at the top of each mode page. */
static GtkWidget *hint_label(const char *markup)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_widget_set_margin_bottom(l, 4);
    return l;
}

/* A bordered "card" grouping with consistent inner padding. */
static GtkWidget *card(GtkWidget *child)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_widget_set_margin_top(child, 12);    gtk_widget_set_margin_bottom(child, 12);
    gtk_widget_set_margin_start(child, 12);  gtk_widget_set_margin_end(child, 12);
    gtk_frame_set_child(GTK_FRAME(frame), child);
    return frame;
}

/* The primary action button for a mode page, right-aligned at the bottom. */
static GtkWidget *action_button(const char *label, const char *css)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, css);
    gtk_widget_set_halign(b, GTK_ALIGN_END);
    gtk_widget_set_margin_top(b, 4);
    return b;
}

/*
 * Free App on application shutdown — after the main loop has stopped, so no
 * queued idle/async callback can still dereference it. Any in-flight file-mode
 * worker is cancelled and joined first; the privileged helper subprocess (if
 * any) keeps running detached to finish its erase, which is intentional.
 */
static void on_shutdown(GApplication *gapp, gpointer u)
{
    (void)gapp;
    App *a = u;
    if (!a) return;
    a->cancel = 1;
    if (a->file_thread) {
        g_thread_join(a->file_thread);   /* consumes the g_thread_new() ref */
        a->file_thread = NULL;
    }
    mech_drop(a);
    g_clear_object(&a->reader);
    g_clear_object(&a->proc);
    g_free(a);
}

static void install_css(void)
{
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p,
        ".sd-title { font-weight: bold; }"
        ".sd-subtitle { font-size: 0.85em; }"
        ".sd-result { padding: 10px; }"
        ".sd-confirm { color: #c0392b; font-weight: bold; }"
        "frame > box { }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static void activate(GtkApplication *gapp, gpointer u)
{
    (void)u;
    App *a = g_new0(App, 1);

    install_css();

    /* Use our installed app icon (share/icons/.../org.sdcheck.gui.svg) for the
       window/taskbar; matches the .desktop Icon= and the application ID. */
    gtk_window_set_default_icon_name("org.sdcheck.gui");

    GtkWidget *win = gtk_application_window_new(gapp);
    a->window = GTK_WINDOW(win);
    gtk_window_set_title(a->window, "SD Card Checker");
    gtk_window_set_default_size(a->window, 620, 700);
    gtk_window_set_icon_name(a->window, "org.sdcheck.gui");

    /* ---- header bar: branded title + global Refresh ---------------------- */
    GtkWidget *hb = gtk_header_bar_new();
    GtkWidget *titlebox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *tl = gtk_label_new("SD Card Checker");
    gtk_widget_add_css_class(tl, "sd-title");
    GtkWidget *sl = gtk_label_new("Spot fake cards · measure speed · format");
    gtk_widget_add_css_class(sl, "sd-subtitle");
    gtk_widget_add_css_class(sl, "dim-label");
    gtk_box_append(GTK_BOX(titlebox), tl);
    gtk_box_append(GTK_BOX(titlebox), sl);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), titlebox);

    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, "Re-scan for removable devices");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), refresh);
    gtk_window_set_titlebar(a->window, hb);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 14);    gtk_widget_set_margin_bottom(root, 14);
    gtk_widget_set_margin_start(root, 14);  gtk_widget_set_margin_end(root, 14);

    /* ---- device card ----------------------------------------------------- */
    GtkWidget *devbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    GtkWidget *picker = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    a->devices = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(a->devices), TRUE);
    a->info_btn = GTK_BUTTON(gtk_button_new_with_label("View info"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(a->info_btn),
        "Show the selected card's identity (read-only)");
    gtk_box_append(GTK_BOX(picker), GTK_WIDGET(a->devices));
    gtk_box_append(GTK_BOX(picker), GTK_WIDGET(a->info_btn));
    gtk_box_append(GTK_BOX(devbox), labeled_row("Device:", picker));

    a->dev_detail = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(a->dev_detail, 0.0);
    gtk_label_set_wrap(a->dev_detail, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(a->dev_detail), "dim-label");
    gtk_box_append(GTK_BOX(devbox), GTK_WIDGET(a->dev_detail));
    gtk_box_append(GTK_BOX(root), card(devbox));

    /* ---- mode switcher + stack ------------------------------------------- */
    a->mode_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(a->mode_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    a->mode_switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(a->mode_switcher), a->mode_stack);
    gtk_widget_set_halign(a->mode_switcher, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), a->mode_switcher);

    /* page 1: safe file test */
    {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(page), hint_label(
            "Writes test files into the card's <b>free space</b> and reads them "
            "back. <b>Safe</b> — your existing files are kept and no administrator "
            "access is needed. Best first check."));
        a->path_entry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(a->path_entry, "/run/media/you/CARD");
        gtk_box_append(GTK_BOX(page),
                       labeled_row("Folder:", GTK_WIDGET(a->path_entry)));
        a->file_btn = GTK_BUTTON(action_button("Start safe test", "suggested-action"));
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->file_btn));
        gtk_stack_add_titled(a->mode_stack, card(page), "file", "Safe test");
    }

    /* page 2: destructive device test */
    {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(page), hint_label(
            "Writes across the <b>whole claimed capacity</b> to expose "
            "fake-capacity cards, and measures real read/write speed. "
            "<span color='#c0392b'><b>Erases the card</b></span> and needs "
            "administrator access."));
        a->full_surface = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Full surface — write every byte, then read it all back to verify (slow)"));
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->full_surface));
        a->iops_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Measure random 4K IOPS (A1/A2 app-performance class)"));
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->iops_check));
        a->passes_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 100, 1));
        gtk_spin_button_set_value(a->passes_spin, 1);
        gtk_widget_set_tooltip_text(GTK_WIDGET(a->passes_spin),
            "Repeat the whole test N times to stress the card");
        gtk_box_append(GTK_BOX(page),
                       labeled_row("Stress passes:", GTK_WIDGET(a->passes_spin)));
        a->confirm_device = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Erase everything on this card — I have a backup"));
        gtk_widget_add_css_class(GTK_WIDGET(a->confirm_device), "sd-confirm");
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->confirm_device));
        a->device_btn = GTK_BUTTON(action_button("Run device test", "destructive-action"));
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->device_btn));
        gtk_stack_add_titled(a->mode_stack, card(page), "device", "Device test");
    }

    /* page 3: low-level format */
    {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(page), hint_label(
            "Wipes the card and optionally lays down a fresh partition and "
            "filesystem. <span color='#c0392b'><b>Erases the card</b></span> and "
            "needs administrator access."));
        const char *methods[] = { "Discard + zero-fill", "Discard (TRIM) only",
                                  "Zero-fill only", NULL };
        a->erase_method = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(methods));
        gtk_box_append(GTK_BOX(page),
                       labeled_row("Erase method:", GTK_WIDGET(a->erase_method)));
        const char *fstypes[] = { "None (erase only)", "exFAT", "FAT32 (vfat)",
                                  "ext4", "NTFS", NULL };
        a->fs_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(fstypes));
        gtk_box_append(GTK_BOX(page),
                       labeled_row("Filesystem:", GTK_WIDGET(a->fs_type)));
        a->make_partition = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Also create a partition table (uses the filesystem above)"));
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->make_partition));
        a->confirm_format = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Erase everything on this card — I have a backup"));
        gtk_widget_add_css_class(GTK_WIDGET(a->confirm_format), "sd-confirm");
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->confirm_format));
        a->format_btn = GTK_BUTTON(action_button("Format card", "destructive-action"));
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->format_btn));
        gtk_stack_add_titled(a->mode_stack, card(page), "format", "Format");
    }

    /* page 4: auto — destructive test, then format the card if it's genuine */
    {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(page), hint_label(
            "Tests the <b>whole claimed capacity</b> and then, <b>only if the card "
            "is genuine</b>, lays down a fresh partition + filesystem so it's ready "
            "to use. <span color='#c0392b'><b>Erases the card</b></span> and needs "
            "administrator access."));
        a->auto_full = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Full surface — write every byte, then read it all back to verify (slow)"));
        gtk_check_button_set_active(a->auto_full, TRUE);   /* auto defaults to thorough */
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->auto_full));
        a->auto_iops = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Measure random 4K IOPS (A1/A2 app-performance class)"));
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->auto_iops));
        a->auto_passes = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 100, 1));
        gtk_spin_button_set_value(a->auto_passes, 1);
        gtk_widget_set_tooltip_text(GTK_WIDGET(a->auto_passes),
            "Repeat the whole test N times to stress the card");
        gtk_box_append(GTK_BOX(page),
                       labeled_row("Stress passes:", GTK_WIDGET(a->auto_passes)));
        const char *autofs[] = { "exFAT", "FAT32 (vfat)", "ext4", "NTFS", NULL };
        a->auto_fs_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(autofs));
        gtk_box_append(GTK_BOX(page),
                       labeled_row("Filesystem:", GTK_WIDGET(a->auto_fs_type)));
        a->confirm_auto = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(
            "Erase everything on this card — I have a backup"));
        gtk_widget_add_css_class(GTK_WIDGET(a->confirm_auto), "sd-confirm");
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->confirm_auto));
        a->auto_btn = GTK_BUTTON(action_button("Test & format", "destructive-action"));
        gtk_box_append(GTK_BOX(page), GTK_WIDGET(a->auto_btn));
        gtk_stack_add_titled(a->mode_stack, card(page), "auto", "Test & format");
    }

    gtk_box_append(GTK_BOX(root), GTK_WIDGET(a->mode_stack));

    /* ---- progress + cancel ---------------------------------------------- */
    GtkWidget *progrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    a->progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_show_text(a->progress, FALSE);
    gtk_widget_set_hexpand(GTK_WIDGET(a->progress), TRUE);
    gtk_widget_set_valign(GTK_WIDGET(a->progress), GTK_ALIGN_CENTER);
    a->cancel_btn = GTK_BUTTON(gtk_button_new_with_label("Cancel"));
    gtk_widget_set_sensitive(GTK_WIDGET(a->cancel_btn), FALSE);
    gtk_box_append(GTK_BOX(progrow), GTK_WIDGET(a->progress));
    gtk_box_append(GTK_BOX(progrow), GTK_WIDGET(a->cancel_btn));
    gtk_box_append(GTK_BOX(root), progrow);

    a->status = GTK_LABEL(gtk_label_new("Ready."));
    gtk_label_set_xalign(a->status, 0.0);
    gtk_widget_add_css_class(GTK_WIDGET(a->status), "dim-label");
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(a->status));

    /* ---- result card ----------------------------------------------------- */
    GtkWidget *resbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_vexpand(resbox, TRUE);

    GtkWidget *reshead = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *reslabel = gtk_label_new("Result");
    gtk_widget_add_css_class(reslabel, "sd-title");
    gtk_widget_set_hexpand(reslabel, TRUE);
    gtk_label_set_xalign(GTK_LABEL(reslabel), 0.0);
    GtkWidget *copy_btn = gtk_button_new_with_label("Copy");
    GtkWidget *save_btn = gtk_button_new_with_label("Save report");
    gtk_widget_set_tooltip_text(copy_btn, "Copy the result text to the clipboard");
    gtk_widget_set_tooltip_text(save_btn, "Save the last result as a JSON report");
    gtk_box_append(GTK_BOX(reshead), reslabel);
    gtk_box_append(GTK_BOX(reshead), copy_btn);
    gtk_box_append(GTK_BOX(reshead), save_btn);
    gtk_box_append(GTK_BOX(resbox), reshead);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    a->results = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_markup(a->results,
        "<i>Pick a device, choose a tab, and run a test. "
        "Results will appear here.</i>");
    gtk_label_set_xalign(a->results, 0.0);
    gtk_label_set_yalign(a->results, 0.0);
    gtk_label_set_wrap(a->results, TRUE);
    gtk_label_set_selectable(a->results, TRUE);   /* drag-select + Ctrl+C */
    gtk_widget_add_css_class(GTK_WIDGET(a->results), "sd-result");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(a->results));
    GtkWidget *resframe = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(resframe), scroll);
    gtk_box_append(GTK_BOX(resbox), resframe);

    gtk_box_append(GTK_BOX(root), resbox);

    gtk_window_set_child(a->window, root);

    /* ---- wiring ---------------------------------------------------------- */
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), a);
    g_signal_connect(a->devices, "notify::selected", G_CALLBACK(on_dev_changed), a);
    g_signal_connect(a->info_btn, "clicked", G_CALLBACK(on_info), a);
    g_signal_connect(a->file_btn, "clicked", G_CALLBACK(on_start_file), a);
    g_signal_connect(a->device_btn, "clicked", G_CALLBACK(on_device), a);
    g_signal_connect(a->format_btn, "clicked", G_CALLBACK(on_format), a);
    g_signal_connect(a->auto_btn, "clicked", G_CALLBACK(on_auto), a);
    g_signal_connect(a->cancel_btn, "clicked", G_CALLBACK(on_cancel), a);
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy), a);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_report), a);
    /* App outlives the window: it is freed in on_shutdown once the main loop
       has stopped, so async helper/mechanism callbacks can never touch freed
       memory if the window is closed mid-operation. */
    g_signal_connect(gapp, "shutdown", G_CALLBACK(on_shutdown), a);

    refresh_devices(a);
    update_detail(a);
    gtk_window_present(a->window);
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.sdcheck.gui", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
