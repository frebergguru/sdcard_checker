#!/usr/bin/env bash
# Quick end-to-end checks for sdcheck.
#
#   ./verify.sh                 # build + selftest + a rootless file-mode test
#                               #   on a loopback FAT image (uses udisksctl)
#   sudo ./verify.sh /dev/sdX   # ALSO runs the destructive device + format
#                               #   paths against a REAL removable card /dev/sdX
#
# WARNING: passing a device runs DESTRUCTIVE tests that ERASE it. Triple-check
# the node with `lsblk` first.
set -euo pipefail
cd "$(dirname "$0")"

echo "== build =="
cmake -B build >/dev/null
cmake --build build >/dev/null
echo "ok"

echo "== selftest =="
./build/sdcheck selftest

echo "== file-mode on a genuine loopback image (rootless via udisksctl) =="
if command -v udisksctl >/dev/null && command -v mkfs.vfat >/dev/null; then
    IMG=$(mktemp --suffix=.img)
    truncate -s 256M "$IMG"
    mkfs.vfat -n SDCHECKTST "$IMG" >/dev/null
    udisksctl loop-setup -f "$IMG" >/dev/null
    LOOP=$(losetup -j "$IMG" | cut -d: -f1)
    sleep 1
    MNT=$(lsblk -nro MOUNTPOINT "$LOOP" | grep -m1 /)
    ./build/sdcheck file "$MNT" --size 1 || true
    udisksctl unmount -b "$LOOP" >/dev/null 2>&1 || true
    udisksctl loop-delete -b "$LOOP" >/dev/null 2>&1 || true
    rm -f "$IMG"
else
    echo "udisksctl/mkfs.vfat not available — skipping"
fi

DEV="${1:-}"
if [[ -n "$DEV" ]]; then
    echo "== DESTRUCTIVE device test on $DEV =="
    ./build/sdcheck device "$DEV" --yes --quick
    echo "== DESTRUCTIVE low-level format on $DEV (+ vfat) =="
    ./build/sdcheck format "$DEV" --yes --both --mkfs vfat
fi
echo "== done =="
