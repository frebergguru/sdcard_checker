# SD Card Checker — prebuilt binaries & system integration

This archive contains a **ready-built** SD Card Checker for x86-64 GNU/Linux:
the CLI, the GTK4 GUI, the privileged D-Bus mechanism, the polkit policy, and the
desktop launcher + icon. No compiler or build step is needed.

> Built on Ubuntu 22.04 (glibc 2.35). It runs on that and **newer** distributions.
> Very old distributions (older glibc) are not supported — build from source for
> those (see the project README).

## Contents

```
usr/bin/sdcheck                                  CLI (also the privileged helper)
usr/bin/sdcheck-gui                              GTK4 GUI
usr/libexec/sdcheck/sdcheck-mechanism            root D-Bus service (Tier 3)
usr/share/applications/org.sdcheck.gui.desktop   desktop launcher
usr/share/icons/.../org.sdcheck.gui.svg          app icon
usr/share/polkit-1/actions/org.sdcheck.policy    branded auth prompt
usr/share/polkit-1/rules.d/49-sdcheck.rules      auth policy (disk group / once per session)
usr/share/dbus-1/system.d/org.sdcheck.Mechanism.conf            bus policy
usr/share/dbus-1/system-services/org.sdcheck.Mechanism.service  on-demand activation
install.sh / uninstall.sh                        installer / remover
```

## Install

```sh
tar xf sdcheck-*-x86_64.tar.gz
cd sdcheck-*-x86_64
sudo ./install.sh
```

It installs into **/usr** (required — the system D-Bus daemon only reads
activation files from `/usr/share/dbus-1/...`), refreshes the icon/desktop caches,
and reloads the system bus. To remove everything: `sudo ./uninstall.sh`.

## Use

```sh
sdcheck list                     # list removable devices
sdcheck info  /dev/sdX           # card identity (CID) — best in a native SD slot
sdcheck file  /run/media/you/CARD   # SAFE free-space test (no root)
sdcheck device /dev/sdX --yes    # DESTRUCTIVE capacity test (auto-elevates via sudo)
sdcheck auto   /dev/sdX --yes    # DESTRUCTIVE: test, then exFAT-partition if genuine
```

Or launch the **SD Card Checker** GUI from your application menu (or run
`sdcheck-gui`). For destructive actions the GUI elevates through polkit; if you
are in the `disk` group it is passwordless, otherwise you get one prompt per
session.

See the full command reference in the project README:
<https://github.com/frebergguru/sdcard_checker>.

## Why this archive *and* the AppImage?

The portable **AppImage** is the easiest way to run the GUI on any modern distro
with zero install — but a self-mounted AppImage **cannot** register the root
D-Bus mechanism or the polkit rule with the system, so its destructive operations
fall back to a plain password prompt every time. **This archive** performs a real
system install, which is what enables the smooth, branded, once-per-session (or
passwordless for the `disk` group) elevation. Install this if you regularly run
the destructive `device` / `format` / `auto` operations; grab the AppImage if you
mainly want the safe test or occasional use.
