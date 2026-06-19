#!/usr/bin/env bash
#
# Build a portable sdcheck-gui AppImage from an already-staged AppDir.
#
# Expects the project to have been installed into ./AppDir via:
#   cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
#   cmake --build build
#   DESTDIR="$PWD/AppDir" cmake --install build
# so that AppDir/usr/{bin,share,libexec}/... already exist.
#
# Produces  SDCardChecker-<version>-x86_64.AppImage  in the current directory.
#
# Usage:  packaging/appimage/build-appimage.sh <version>
#
# Notes
# - GTK4 is bundled via linuxdeploy-plugin-gtk with DEPLOY_GTK_VERSION=4. This is
#   the most fragile part; if a GTK module/loader is missing at runtime it will
#   show up here.
# - Runs the helper AppImages in extract-and-run mode so no FUSE is required in
#   CI containers.
set -euo pipefail

VERSION="${1:-dev}"
ARCH="x86_64"
APPDIR="${APPDIR:-AppDir}"

if [ ! -d "${APPDIR}/usr/bin" ]; then
    echo "error: ${APPDIR}/usr/bin not found — stage the install into ${APPDIR} first." >&2
    exit 1
fi

export APPIMAGE_EXTRACT_AND_RUN=1
export DEPLOY_GTK_VERSION=4          # tell the gtk plugin to target GTK4
export ARCH

tools_dir="$(mktemp -d)"
fetch() { wget -q -O "$2" "$1"; chmod +x "$2"; }

echo "==> Fetching linuxdeploy + GTK plugin + appimagetool"
base="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
gtkbase="https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master"
aibase="https://github.com/AppImage/appimagetool/releases/download/continuous"
fetch "${base}/linuxdeploy-${ARCH}.AppImage"            "${tools_dir}/linuxdeploy"
fetch "${gtkbase}/linuxdeploy-plugin-gtk.sh"            "${tools_dir}/linuxdeploy-plugin-gtk.sh"
fetch "${aibase}/appimagetool-${ARCH}.AppImage"         "${tools_dir}/appimagetool"
export PATH="${tools_dir}:${PATH}"

echo "==> Running linuxdeploy (bundling GTK4 + dependencies)"
OUTPUT="SDCardChecker-${VERSION}-${ARCH}.AppImage" \
linuxdeploy \
    --appdir "${APPDIR}" \
    --executable "${APPDIR}/usr/bin/sdcheck-gui" \
    --executable "${APPDIR}/usr/bin/sdcheck" \
    --desktop-file "${APPDIR}/usr/share/applications/org.sdcheck.gui.desktop" \
    --icon-file "${APPDIR}/usr/share/icons/hicolor/scalable/apps/org.sdcheck.gui.svg" \
    --plugin gtk \
    --output appimage

echo "==> Built: SDCardChecker-${VERSION}-${ARCH}.AppImage"
