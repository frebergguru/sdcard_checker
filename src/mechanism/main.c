/*
 * sdcheck-mechanism — privileged D-Bus system service (Tier 3).
 *
 * The unprivileged GUI calls this over the system bus instead of shelling out
 * to pkexec. For each request the service checks polkit authorization for the
 * custom action org.sdcheck.manage-device against the *calling* client, so the
 * user sees a branded, per-operation prompt (and the disk group / one-prompt-
 * per-session policy from the polkit rule still applies). Only this small,
 * headless process runs as root; it is D-Bus activated on demand and exits
 * after a short idle period.
 *
 * Interface org.sdcheck.Mechanism1:
 *   Test(s node, b full)                 - run the destructive capacity test
 *   Format(s node, s method, s mkfs)     - low-level erase (method discard/zero/both)
 *   Auto(s node, ...)                    - test, then format if the card is genuine
 *   Cancel()                             - abort the running operation
 *   signal Progress(i phase, t done, t total, d bps)
 *   signal Finished(i verdict, t announced, t real,
 *                   d avgW, d minW, d maxW, d avgR, d minR, d maxR,
 *                   i class, s message)
 */
#define _GNU_SOURCE
#include "sdcheck/sdcheck.h"

#include <gio/gio.h>
#include <polkit/polkit.h>
#include <string.h>

#define BUS_NAME  "org.sdcheck.Mechanism"
#define OBJ_PATH  "/org/sdcheck/Mechanism"
#define IFACE     "org.sdcheck.Mechanism1"
#define ACTION_ID "org.sdcheck.manage-device"
#define IDLE_SECONDS 30

static GMainLoop      *loop;
static GDBusConnection *bus;
static PolkitAuthority *authority;

static volatile int g_cancel;
static gboolean     g_busy;        /* an operation is running             */
static guint        g_idle_source; /* idle-exit timer, 0 when disarmed    */

/* ---- idle-exit handling (don't linger as root) -------------------------- */

static gboolean idle_quit(gpointer u)
{
    (void)u;
    g_idle_source = 0;
    if (!g_busy) g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void disarm_idle(void)
{
    if (g_idle_source) { g_source_remove(g_idle_source); g_idle_source = 0; }
}

static void arm_idle(void)
{
    disarm_idle();
    if (!g_busy) g_idle_source = g_timeout_add_seconds(IDLE_SECONDS, idle_quit, NULL);
}

/* ---- signal emission (marshalled onto the main thread) ------------------ */

typedef struct {
    gboolean finished;
    /* progress */
    int phase; guint64 done, total; double bps;
    /* finished */
    sdcheck_result r;
} Emit;

static gboolean do_emit(gpointer data)
{
    Emit *e = data;
    if (!e->finished) {
        g_dbus_connection_emit_signal(bus, NULL, OBJ_PATH, IFACE, "Progress",
            g_variant_new("(ittd)", e->phase, e->done, e->total, e->bps), NULL);
    } else {
        const sdcheck_result *r = &e->r;
        GVariantBuilder regions;
        g_variant_builder_init(&regions, G_VARIANT_TYPE("a(tt)"));
        for (int i = 0; i < r->bad_region_count; i++)
            g_variant_builder_add(&regions, "(tt)",
                                  r->bad_regions[i].offset, r->bad_regions[i].length);
        g_dbus_connection_emit_signal(bus, NULL, OBJ_PATH, IFACE, "Finished",
            g_variant_new("(ittddddddiddiiia(tt)s)",
                (int)r->verdict, r->announced_bytes, r->real_bytes,
                r->avg_write_bps, r->min_write_bps, r->max_write_bps,
                r->avg_read_bps,  r->min_read_bps,  r->max_read_bps,
                (int)r->speed_class,
                r->read_iops, r->write_iops, (int)r->app_class,
                r->passes_done, r->passes_total,
                &regions, r->message), NULL);
        g_busy = FALSE;
        arm_idle();            /* allow shutdown again now that work is done */
    }
    g_free(e);
    return G_SOURCE_REMOVE;
}

static void emit_progress(sdcheck_phase phase, uint64_t done, uint64_t total,
                          double bps, void *user)
{
    (void)user;
    Emit *e = g_new0(Emit, 1);
    e->finished = FALSE;
    e->phase = (int)phase; e->done = done; e->total = total; e->bps = bps;
    g_idle_add(do_emit, e);
}

/* ---- worker thread ------------------------------------------------------ */

typedef enum { JOB_TEST = 0, JOB_FORMAT, JOB_AUTO } JobKind;

typedef struct {
    JobKind  kind;
    char     node[64];
    gboolean full;                 /* test / auto  */
    int      passes;               /* test / auto  */
    gboolean measure_iops;         /* test / auto  */
    sdcheck_fmt_method method;     /* format / auto */
    char     mkfs[16];             /* format / auto */
    gboolean partition;            /* format: partition + mkfs */
} Job;

static gpointer worker(gpointer data)
{
    Job *j = data;
    Emit *e = g_new0(Emit, 1);
    e->finished = TRUE;

    if (j->kind == JOB_FORMAT) {
        sdcheck_format_options fo; memset(&fo, 0, sizeof(fo));
        fo.method = j->method;
        fo.confirm_destroy = 1;
        fo.make_partition = j->partition;
        g_strlcpy(fo.mkfs_type, j->mkfs, sizeof(fo.mkfs_type));
        sdcheck_format_device(j->node, &fo, emit_progress, NULL, &g_cancel, &e->r);
    } else if (j->kind == JOB_AUTO) {
        sdcheck_options o; sdcheck_options_default(&o);
        o.confirm_destroy = 1;
        o.full_surface = j->full;
        o.passes = j->passes;
        o.measure_iops = j->measure_iops;
        sdcheck_format_options fo; memset(&fo, 0, sizeof(fo));
        fo.method = j->method;
        fo.make_partition = 1;
        g_strlcpy(fo.mkfs_type, j->mkfs, sizeof(fo.mkfs_type));
        sdcheck_run_auto(j->node, &o, &fo, emit_progress, NULL, &g_cancel, &e->r);
    } else {
        sdcheck_options o; sdcheck_options_default(&o);
        o.confirm_destroy = 1;
        o.full_surface = j->full;
        o.passes = j->passes;
        o.measure_iops = j->measure_iops;
        sdcheck_run_device(j->node, &o, emit_progress, NULL, &g_cancel, &e->r);
    }

    g_idle_add(do_emit, e);        /* emit Finished + clear busy on main loop */
    g_free(j);
    return NULL;
}

/* ---- polkit authorization ---------------------------------------------- */

static gboolean caller_authorized(const char *sender)
{
    PolkitSubject *subject = polkit_system_bus_name_new(sender);
    GError *err = NULL;
    PolkitAuthorizationResult *res = polkit_authority_check_authorization_sync(
        authority, subject, ACTION_ID, NULL,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION, NULL, &err);

    gboolean ok = (res && polkit_authorization_result_get_is_authorized(res));
    if (res) g_object_unref(res);
    g_object_unref(subject);
    g_clear_error(&err);
    return ok;
}

/* ---- D-Bus method dispatch --------------------------------------------- */

static sdcheck_fmt_method parse_method(const char *m)
{
    if (!g_strcmp0(m, "discard")) return SDCHECK_FMT_DISCARD;
    if (!g_strcmp0(m, "zero"))    return SDCHECK_FMT_ZERO;
    return SDCHECK_FMT_DISCARD_THEN_ZERO;
}

/* Basic shape check; the real safety gate (removable-only, unmount, …) runs in
   the core once we start the job. */
static gboolean node_ok(const char *node)
{
    return node && g_str_has_prefix(node, "/dev/") && !strstr(node, "..");
}

static void handle_method(GDBusConnection *conn, const char *sender,
                          const char *object_path, const char *interface_name,
                          const char *method_name, GVariant *params,
                          GDBusMethodInvocation *inv, gpointer user_data)
{
    (void)conn; (void)object_path; (void)interface_name; (void)user_data;
    disarm_idle();

    if (!g_strcmp0(method_name, "Cancel")) {
        g_cancel = 1;
        g_dbus_method_invocation_return_value(inv, NULL);
        if (!g_busy) arm_idle();
        return;
    }

    if (g_busy) {
        g_dbus_method_invocation_return_dbus_error(inv, IFACE ".Busy",
            "An operation is already in progress.");
        return;
    }

    if (!caller_authorized(sender)) {
        g_dbus_method_invocation_return_dbus_error(inv, IFACE ".NotAuthorized",
            "Authentication failed or was cancelled; the device was not touched.");
        arm_idle();
        return;
    }

    Job *j = g_new0(Job, 1);
    if (!g_strcmp0(method_name, "Test")) {
        const char *node; gboolean full, iops; gint passes;
        g_variant_get(params, "(&sbib)", &node, &full, &passes, &iops);
        j->kind = JOB_TEST; j->full = full;
        j->passes = passes; j->measure_iops = iops;
        g_strlcpy(j->node, node ? node : "", sizeof(j->node));
    } else if (!g_strcmp0(method_name, "Format")) {
        const char *node, *meth, *mkfs; gboolean partition;
        g_variant_get(params, "(&s&s&sb)", &node, &meth, &mkfs, &partition);
        j->kind = JOB_FORMAT; j->method = parse_method(meth); j->partition = partition;
        g_strlcpy(j->node, node ? node : "", sizeof(j->node));
        g_strlcpy(j->mkfs, mkfs ? mkfs : "", sizeof(j->mkfs));
    } else if (!g_strcmp0(method_name, "Auto")) {
        const char *node, *mkfs, *meth; gboolean full, iops; gint passes;
        g_variant_get(params, "(&sbib&s&s)", &node, &full, &passes, &iops, &mkfs, &meth);
        j->kind = JOB_AUTO; j->full = full;
        j->passes = passes; j->measure_iops = iops;
        j->method = parse_method(meth);
        g_strlcpy(j->node, node ? node : "", sizeof(j->node));
        g_strlcpy(j->mkfs, mkfs ? mkfs : "", sizeof(j->mkfs));
    } else {
        g_free(j);
        g_dbus_method_invocation_return_dbus_error(inv, IFACE ".UnknownMethod",
            "Unknown method.");
        arm_idle();
        return;
    }

    if (!node_ok(j->node)) {
        g_free(j);
        g_dbus_method_invocation_return_dbus_error(inv, IFACE ".BadDevice",
            "Device must be an absolute /dev path.");
        arm_idle();
        return;
    }

    g_cancel = 0;
    g_busy = TRUE;
    g_dbus_method_invocation_return_value(inv, NULL);   /* ack; progress via signals */

    GThread *t = g_thread_new("sdcheck-op", worker, j);
    g_thread_unref(t);
}

static const GDBusInterfaceVTable vtable = { handle_method, NULL, NULL, { 0 } };

/* ---- bus lifecycle ------------------------------------------------------ */

static void on_name_lost(GDBusConnection *c, const char *name, gpointer u)
{
    (void)c; (void)name; (void)u;
    /* Could not own the name (already running, or policy refused): bail out. */
    if (loop) g_main_loop_quit(loop);
}

int main(void)
{
    GError *err = NULL;

    loop = g_main_loop_new(NULL, FALSE);

    authority = polkit_authority_get_sync(NULL, &err);
    if (!authority) {
        g_printerr("sdcheck-mechanism: polkit unavailable: %s\n",
                   err ? err->message : "?");
        return 1;
    }

    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!bus) {
        g_printerr("sdcheck-mechanism: no system bus: %s\n", err ? err->message : "?");
        return 1;
    }

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(
        "<node><interface name='" IFACE "'>"
        "  <method name='Test'>"
        "    <arg type='s' name='node' direction='in'/>"
        "    <arg type='b' name='full' direction='in'/>"
        "    <arg type='i' name='passes' direction='in'/>"
        "    <arg type='b' name='iops' direction='in'/>"
        "  </method>"
        "  <method name='Format'>"
        "    <arg type='s' name='node' direction='in'/>"
        "    <arg type='s' name='method' direction='in'/>"
        "    <arg type='s' name='mkfs' direction='in'/>"
        "    <arg type='b' name='partition' direction='in'/>"
        "  </method>"
        "  <method name='Auto'>"
        "    <arg type='s' name='node' direction='in'/>"
        "    <arg type='b' name='full' direction='in'/>"
        "    <arg type='i' name='passes' direction='in'/>"
        "    <arg type='b' name='iops' direction='in'/>"
        "    <arg type='s' name='mkfs' direction='in'/>"
        "    <arg type='s' name='method' direction='in'/>"
        "  </method>"
        "  <method name='Cancel'/>"
        "  <signal name='Progress'>"
        "    <arg type='i' name='phase'/><arg type='t' name='done'/>"
        "    <arg type='t' name='total'/><arg type='d' name='bps'/>"
        "  </signal>"
        "  <signal name='Finished'>"
        "    <arg type='i' name='verdict'/><arg type='t' name='announced'/>"
        "    <arg type='t' name='real'/>"
        "    <arg type='d' name='avgw'/><arg type='d' name='minw'/><arg type='d' name='maxw'/>"
        "    <arg type='d' name='avgr'/><arg type='d' name='minr'/><arg type='d' name='maxr'/>"
        "    <arg type='i' name='class'/>"
        "    <arg type='d' name='riops'/><arg type='d' name='wiops'/>"
        "    <arg type='i' name='appclass'/>"
        "    <arg type='i' name='passes_done'/><arg type='i' name='passes_total'/>"
        "    <arg type='a(tt)' name='bad_regions'/>"
        "    <arg type='s' name='message'/>"
        "  </signal>"
        "</interface></node>", &err);
    if (!node) {
        g_printerr("sdcheck-mechanism: bad introspection XML: %s\n",
                   err ? err->message : "?");
        return 1;
    }

    if (!g_dbus_connection_register_object(bus, OBJ_PATH, node->interfaces[0],
                                           &vtable, NULL, NULL, &err)) {
        g_printerr("sdcheck-mechanism: register failed: %s\n", err ? err->message : "?");
        return 1;
    }

    g_bus_own_name_on_connection(bus, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 NULL, on_name_lost, NULL, NULL);

    arm_idle();
    g_main_loop_run(loop);

    g_dbus_node_info_unref(node);
    return 0;
}
