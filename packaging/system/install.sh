#!/usr/bin/env bash
#
# Install the prebuilt SD Card Checker binaries + system integration.
#
# This archive contains a ready-built install tree under ./usr — the CLI, GUI,
# the privileged D-Bus mechanism, the polkit policy, and the desktop entry/icon.
# This script copies it into the system prefix and activates it; no compiler or
# build step is required.
#
#   sudo ./install.sh
#
# The prefix is fixed to /usr: the polkit rule and the system D-Bus activation
# file were generated with absolute /usr paths, and the system bus only reads
# activation files from /usr/share/dbus-1/... — so /usr is required for the
# Tier-3 mechanism (and thus the smooth, branded elevation) to work.
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

if [ "$(id -u)" -ne 0 ]; then
    echo "This installer must run as root:  sudo ./install.sh" >&2
    exit 1
fi

if [ ! -d usr ]; then
    echo "error: ./usr not found next to this script — extract the full archive first." >&2
    exit 1
fi

PREFIX=/usr

echo "==> Installing into ${PREFIX}"
cp -a usr/. "${PREFIX}/"

# Refresh the icon cache + desktop database so the launcher/icon appear at once.
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -qtf "${PREFIX}/share/icons/hicolor" 2>/dev/null || true
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database "${PREFIX}/share/applications" 2>/dev/null || true

# Reload the system bus so the Tier-3 mechanism's policy + activation are seen.
if command -v systemctl >/dev/null 2>&1; then
    echo "==> Reloading system D-Bus"
    systemctl reload dbus 2>/dev/null \
        || systemctl reload dbus.service 2>/dev/null \
        || echo "    (could not reload dbus; a reboot will apply it)"
fi

# Drop any stale mechanism so the next operation activates the new binary.
if pgrep -x sdcheck-mechanism >/dev/null 2>&1; then
    pkill -x sdcheck-mechanism 2>/dev/null || true
fi

echo
echo "==> Done."
echo "    CLI:  sdcheck list"
echo "    GUI:  sdcheck-gui   (also in your application menu)"
echo "    Uninstall:  sudo ./uninstall.sh"
