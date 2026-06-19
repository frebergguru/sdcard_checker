#!/usr/bin/env bash
#
# Clean, configure, build, install, and activate sdcheck from scratch.
#
#   ./install.sh [INSTALL_PREFIX]     # default prefix: /usr
#
# The build runs as you; the install and D-Bus reload use sudo (you'll be
# prompted for your password). Pass a prefix to install elsewhere, e.g.
#   ./install.sh /usr/local
#
set -euo pipefail

# Always operate from the project root (the directory this script lives in).
cd "$(dirname "$(readlink -f "$0")")"

PREFIX="${1:-/usr}"
BUILD="build"

echo "==> Cleaning ${BUILD}/"
rm -rf "${BUILD}"

echo "==> Configuring (install prefix: ${PREFIX})"
cmake -B "${BUILD}" -DCMAKE_INSTALL_PREFIX="${PREFIX}"

echo "==> Building"
cmake --build "${BUILD}"

echo "==> Installing (requires root — you may be prompted for your password)"
sudo cmake --install "${BUILD}"

# Reload the system bus so the Tier-3 D-Bus mechanism's bus policy and on-demand
# activation file are picked up immediately. Harmless if the mechanism was not
# built (no GIO/polkit) or if your init isn't systemd — hence best-effort.
if command -v systemctl >/dev/null 2>&1; then
    echo "==> Reloading system D-Bus"
    sudo systemctl reload dbus 2>/dev/null \
        || sudo systemctl reload dbus.service 2>/dev/null \
        || echo "    (could not reload dbus; a reboot or manual reload will apply it)"
fi

# Refresh the icon-theme cache and desktop database so the GUI's launcher and
# icon appear immediately (best-effort; harmless if the tools aren't present).
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    sudo gtk-update-icon-cache -qtf "${PREFIX}/share/icons/hicolor" 2>/dev/null || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    sudo update-desktop-database "${PREFIX}/share/applications" 2>/dev/null || true
fi

# Stop any mechanism left running from a previous install. It is D-Bus-activated
# on demand and exits when idle, but an instance still alive keeps owning the bus
# name — so without this the next operation would be served by the OLD binary
# (whose signal layout may differ from the just-installed GUI) until it idle-exits.
# Killing it forces a fresh activation of the new binary on the next call.
if pgrep -x sdcheck-mechanism >/dev/null 2>&1; then
    echo "==> Stopping the previous root mechanism (re-activates fresh on next use)"
    sudo pkill -x sdcheck-mechanism 2>/dev/null || true
fi

echo
echo "==> Done. Installed under ${PREFIX}."
echo "    CLI:  sdcheck list"
echo "    GUI:  sdcheck-gui"
echo "    Uninstall:  sudo cmake --build ${BUILD} --target uninstall"
