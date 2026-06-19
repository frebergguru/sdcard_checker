#!/usr/bin/env bash
#
# Remove a system install made by ./install.sh (prefix /usr).
#
#   sudo ./uninstall.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "This must run as root:  sudo ./uninstall.sh" >&2
    exit 1
fi

PREFIX=/usr

# Stop the root mechanism if it is currently running.
pkill -x sdcheck-mechanism 2>/dev/null || true

rm -f  "${PREFIX}/bin/sdcheck" \
       "${PREFIX}/bin/sdcheck-gui" \
       "${PREFIX}/libexec/sdcheck/sdcheck-mechanism" \
       "${PREFIX}/share/applications/org.sdcheck.gui.desktop" \
       "${PREFIX}/share/icons/hicolor/scalable/apps/org.sdcheck.gui.svg" \
       "${PREFIX}/share/polkit-1/actions/org.sdcheck.policy" \
       "${PREFIX}/share/polkit-1/rules.d/49-sdcheck.rules" \
       "${PREFIX}/share/dbus-1/system.d/org.sdcheck.Mechanism.conf" \
       "${PREFIX}/share/dbus-1/system-services/org.sdcheck.Mechanism.service"
rmdir  "${PREFIX}/libexec/sdcheck" 2>/dev/null || true

command -v systemctl >/dev/null 2>&1 && systemctl reload dbus 2>/dev/null || true

echo "Removed SD Card Checker from ${PREFIX}."
