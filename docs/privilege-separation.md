# Design sketch: privilege-separated GUI (root helper)

**Status:** Tiers 1–3 **implemented** (Tier 3 needs on-hardware testing — see
below). Tier 1 — the GUI runs the `sdcheck helper` subcommand via `pkexec` and
streams its stdout (`src/cli/main.c` `cmd_helper`, `src/gui/main.c`
`start_via_pkexec` / `on_helper_line`); the `mkfs` shell-out became a direct
`execvp` (`src/core/format.c`). Tier 2 — a polkit rules file
(`dist/49-sdcheck.rules.in`) grants the `disk` group passwordless use and gives
everyone else one admin prompt per session. Tier 3 — a root D-Bus system service
(`src/mechanism/main.c`) that checks the custom action `org.sdcheck.manage-device`
for a branded, per-operation prompt; the GUI prefers it
(`start_via_mechanism`) and transparently falls back to the pkexec helper when it
is not installed. Packaging: `dist/org.sdcheck.policy` (action),
`dist/org.sdcheck.Mechanism.conf` (bus policy),
`dist/org.sdcheck.Mechanism.service.in` (on-demand activation), all installed by
CMake.

The privileged surface covers three destructive operations, all routed through
the same tiers and the single `org.sdcheck.manage-device` action: `device` (test),
`format` (erase/partition/mkfs), and `auto` (test, then format the card as exFAT
only if it verifies genuine). Tier 1 exposes them as `sdcheck helper <op> <node>`;
Tier 3 as the D-Bus methods `Test`, `Format`, and `Auto` on `org.sdcheck.Mechanism1`.

> Tier 3 is compile-verified only; it could not be exercised in the dev sandbox
> (no system D-Bus / polkit / display). Test it on a real system per the README.
**Problem it solves:** the current GUI relaunches its *entire self* as root via
`pkexec`. That is (a) blocked on many **Wayland** compositors — root can't reach
the user's Wayland socket, so the elevated GUI gets no display — and (b) poor
hygiene: a large GTK process running as root is a big attack surface for a job
whose privileged part is a handful of block-device `ioctl`/`write` calls.

The fix is classic privilege separation: **the GUI never runs as root.** It
stays an ordinary user process and shells out to a tiny, *non-graphical* root
helper for the few operations that need it. A headless root process has no
display dependency, so the Wayland problem disappears entirely.

```
  ┌────────────────────┐    pkexec spawns      ┌──────────────────────┐
  │  sdcheck-gui        │ ───────────────────▶  │  sdcheck-helper      │
  │  (your uid, GTK)    │   argv: op + devnode  │  (root, no display)  │
  │                     │                       │                      │
  │  reads stdout  ◀────┼───── P/D/M lines ─────┼──  libsdcheck core   │
  │  writes stdin  ─────┼────── "cancel" ──────▶│  (device/format)     │
  └────────────────────┘   exit code = verdict  └──────────────────────┘
```

The core library (`libsdcheck`) does **not** change: it already takes a progress
callback and a `volatile int *cancel`. Only the front-end wiring moves.

---

## 1. The helper

A small privileged entry point. Cheapest option: a hidden subcommand on the
existing CLI binary (no new build target), e.g. `sdcheck helper <op> <node>`.
A dedicated `sdcheck-helper` installed under `libexec` is slightly cleaner for
polkit rules (stable path to match on) — either works.

It speaks a line-based, flushed protocol on **stdout** so the GUI can parse it,
and reads cancellation from **stdin** (works across the uid boundary, unlike
signals — pkexec inherits our stdio pipes):

```
P <phase:int> <done:u64> <total:u64> <bps:double>     # progress, many
D <verdict> <announced> <real> <avgW> <minW> <maxW> <avgR> <minR> <maxR> <class>
M <message text to end of line>                        # final, once
```

```c
/* --- in the CLI binary: sdcheck helper <device|format> <node> [flags] --- */
static volatile int h_cancel = 0;

static void *cancel_reader(void *u) {           /* stdin → cancel flag */
    (void)u; char line[64];
    while (fgets(line, sizeof line, stdin))
        if (!strncmp(line, "cancel", 6)) { h_cancel = 1; break; }
    return NULL;
}

static void h_progress(sdcheck_phase ph, uint64_t done, uint64_t total,
                       double bps, void *u) {
    (void)u;
    printf("P %d %" PRIu64 " %" PRIu64 " %.3f\n", (int)ph, done, total, bps);
    fflush(stdout);                              /* unbuffered: GUI sees it live */
}

static int cmd_helper(int argc, char **argv) {
    if (argc < 2) return 1;
    const char *op = argv[0], *node = argv[1];

    pthread_t t; pthread_create(&t, NULL, cancel_reader, NULL);
    pthread_detach(t);

    sdcheck_result r;
    if (!strcmp(op, "format")) {
        sdcheck_format_options fo; memset(&fo, 0, sizeof fo);
        fo.method = SDCHECK_FMT_DISCARD_THEN_ZERO; fo.confirm_destroy = 1;
        /* parse --mkfs/--zero/etc. from argv+2 here */
        sdcheck_format_device(node, &fo, h_progress, NULL, &h_cancel, &r);
    } else {
        sdcheck_options o; sdcheck_options_default(&o); o.confirm_destroy = 1;
        /* parse --full from argv+2 here */
        sdcheck_run_device(node, &o, h_progress, NULL, &h_cancel, &r);
    }

    printf("D %d %" PRIu64 " %" PRIu64 " %.3f %.3f %.3f %.3f %.3f %.3f %d\n",
           (int)r.verdict, r.announced_bytes, r.real_bytes,
           r.avg_write_bps, r.min_write_bps, r.max_write_bps,
           r.avg_read_bps,  r.min_read_bps,  r.max_read_bps, (int)r.speed_class);
    printf("M %s\n", r.message);
    fflush(stdout);
    return r.verdict == SDCHECK_FAKE ? 2 : (r.verdict == SDCHECK_ERROR ? 1 : 0);
}
```

The existing `sdcheck_guard_destructive()` runs *inside* the helper — i.e. as
root, which is where the removable-only / unmount / not-system-disk checks
belong. `confirm_destroy = 1` is set by the helper because reaching it already
means the GUI's checkbox was ticked; the privilege boundary is the password, not
the flag.

---

## 2. GUI side (GIO `GSubprocess`, all on the main thread)

Replace the in-process worker thread *for the two destructive ops only* (the
safe file test still runs in-process — no root needed) with a subprocess whose
stdout we read asynchronously. GIO async callbacks fire on the GTK main loop, so
there is **no more worker thread and no `g_idle_add` marshalling** for these.

```c
static void start_privileged(App *a, const char *op, const char *node) {
    GError *err = NULL;
    a->proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err,
        "pkexec", helper_path(), "helper", op, node, NULL);   /* argv, no shell */
    if (!a->proc) { show_error(a, err->message); g_clear_error(&err); return; }

    a->reader = g_data_input_stream_new(g_subprocess_get_stdout_pipe(a->proc));
    g_data_input_stream_read_line_async(a->reader, G_PRIORITY_DEFAULT, NULL,
                                        on_line, a);            /* self-rearming */
    g_subprocess_wait_check_async(a->proc, NULL, on_finished, a);
}

static void on_line(GObject *src, GAsyncResult *res, gpointer u) {
    App *a = u; gsize len;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src),
                                                      res, &len, NULL);
    if (!line) return;                       /* EOF: on_finished handles the rest */
    switch (line[0]) {
        case 'P': /* sscanf → gtk_progress_bar_set_fraction + status label */ break;
        case 'D': /* sscanf → stash fields for the result panel            */ break;
        case 'M': /* render verdict markup (reuse apply_done formatting)    */ break;
    }
    g_free(line);
    g_data_input_stream_read_line_async(a->reader, G_PRIORITY_DEFAULT, NULL,
                                        on_line, a);            /* read next line */
}

static void on_finished(GObject *src, GAsyncResult *res, gpointer u) {
    App *a = u; GError *err = NULL;
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(src), res, &err)) {
        int code = g_subprocess_get_exit_status(a->proc);
        if (code == 126 || code == 127)      /* pkexec: denied / dismissed */
            show_error(a, "Not elevated — wrong password or cancelled. "
                          "The card was NOT modified.");
        else
            show_error(a, err ? err->message : "Helper failed.");
        g_clear_error(&err);
    }
    /* else: D/M lines already populated the result panel */
    g_clear_object(&a->proc); g_clear_object(&a->reader);
}
```

**Cancel** button writes one line to the helper's stdin — no signals, no uid
games:

```c
GOutputStream *in = g_subprocess_get_stdin_pipe(a->proc);
g_output_stream_write_all(in, "cancel\n", 7, NULL, NULL, NULL);
```

**Deletions this enables:** `relaunch_as_root()`, `ensure_root_or_relaunch()`,
`elevated_child_exited()`, the `g_application_hold/release` dance, and the
window hide/show handoff all go away. The GUI is single-window because it never
relaunches. The wrong-password message survives as the `on_finished` 126/127
branch above.

---

## 3. polkit integration

**Tier 1 — plain pkexec (no policy file, ship today).** pkexec uses the built-in
action `org.freedesktop.policykit.exec`; the dialog reads *"Authentication is
required to run sdcheck-helper as the super user."* Generic but correct, and
needs nothing installed.

**Tier 2 — a rules file** to tune retention or restrict who may do it, matching
on the helper's path:

```javascript
/* /etc/polkit-1/rules.d/49-sdcheck.rules */
polkit.addRule(function(action, subject) {
    if (action.id == "org.freedesktop.policykit.exec" &&
        action.lookup("program") == "/usr/lib/sdcheck/sdcheck-helper" &&
        subject.isInGroup("disk")) {
        return polkit.Result.AUTH_ADMIN_KEEP;   /* one auth per session */
    }
});
```

**Tier 3 — a custom action + D-Bus mechanism** (gold-plated; only if you want a
branded auth message/icon and per-operation policy). The helper becomes a small
system D-Bus service that calls `polkit_authority_check_authorization()` for your
own action id, instead of being launched by pkexec:

```xml
<!-- /usr/share/polkit-1/actions/org.sdcheck.policy -->
<policyconfig>
  <action id="org.sdcheck.manage-device">
    <description>Erase or test a removable storage device</description>
    <message>Authentication is required to test or erase a removable card</message>
    <defaults>
      <allow_active>auth_admin</allow_active>
      <allow_inactive>auth_admin</allow_inactive>
    </defaults>
  </action>
</policyconfig>
```

This is a substantial step up in machinery (systemd unit + D-Bus conf + interface
+ object lifetime). Recommend Tier 1 first; add Tier 2 if users complain about
re-typing; only reach for Tier 3 if the polish is worth a daemon.

---

## 4. Build / install (CMake)

```cmake
# dedicated helper (or skip and reuse the sdcheck binary's `helper` subcommand)
add_executable(sdcheck-helper src/helper/main.c)
target_link_libraries(sdcheck-helper PRIVATE sdcheck_core Threads::Threads)

include(GNUInstallDirs)
install(TARGETS sdcheck sdcheck-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS sdcheck-helper      RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/sdcheck)
# Tier 2/3 only:
install(FILES dist/49-sdcheck.rules DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/polkit-1/rules.d)
install(FILES dist/org.sdcheck.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
```

The GUI resolves the helper path from its own install prefix (or falls back to
`/proc/self/exe` + `helper` when running from the build tree).

---

## 5. Security notes

- **argv, never a shell.** `GSubprocess`/`pkexec` take an argv vector — no string
  interpolation, so a device node can't smuggle shell metacharacters.
- **The helper re-validates everything itself**, as root, via the existing guard:
  removable-only (blocks system disks), whole-disk, unmount-or-refuse. It trusts
  nothing from the unprivileged caller except "which removable device."
- **Capability granted = wipe any *removable* device**, gated by the admin
  password. That's the intended power; the removable guard is what keeps it from
  becoming "wipe `/`."
- **`format.c` uses `system("mkfs.%s ...")`.** Fine today (allow-listed type,
  vetted node), but since it now runs as root, prefer swapping it for a direct
  `execvp("mkfs.<type>", ...)` in a fork to drop the shell entirely.

---

## 6. Effort estimate

| Piece                                   | Size  |
|-----------------------------------------|-------|
| Helper subcommand + stdout/stdin protocol | ~80 LOC |
| GUI: GSubprocess + async line reader + cancel | ~120 LOC, **removes** ~90 |
| CMake install rules                     | ~10 LOC |
| Tier 2 rules file                       | ~8 LOC  |
| Tier 3 D-Bus mechanism (optional)       | ~300 LOC + unit/conf |

Net: Tiers 1–2 are a roughly break-even change in line count that deletes the
fragile relaunch path and **makes the GUI work under Wayland**. Tier 3 is the
only large piece and is optional polish.
```
