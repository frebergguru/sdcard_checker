# sdcheck — SD / USB flash card authenticity checker

Detects **fake-capacity** flash cards (the kind that report 256 GB but really
hold a few GB) by writing a deterministic pattern across the claimed capacity
and reading it back. Also **measures read/write speed**, reads the card's
**identity registers (CID)**, and can **low-level format / partition** a card.

Ships as a shared core library (`libsdcheck`) with two front-ends:

- `sdcheck` — command-line tool (always built)
- `sdcheck-gui` — GTK4 graphical tool (built if GTK4 is installed)

## How it detects fakes

Counterfeit cards lie about size; writes past the real flash are silently
dropped or wrapped onto low addresses. The tool writes a pattern that is a pure
function of `(seed, byte offset)` — each region's data is unique to its
location — then reads it back. Wherever read-back diverges marks where real
storage ends. The seed is **randomized per run**, so the written bytes are
unpredictable: a fake that returns all-zeros, all-ones, cached data, or its own
"random" filler when read past its real capacity won't match and is flagged. Speed is measured during the same I/O (counterfeits are often
slow too) and matched to nominal SD speed classes (Class 2–10, U1/U3, V6–V90).

Speed numbers are measured over ≥32 MiB windows that include periodic flushes to
the card, so the reported **avg / min / max** reflect real media throughput
rather than transient page-cache speed, and the live progress readout is
smoothed the same way.

## Dependencies

**Build:**

- CMake ≥ 3.16 and a C11 compiler
- *(optional, GUI)* GTK4 — `gtk4`
- *(optional, Tier-3 elevation)* GLib/GIO — `gio-2.0` — and polkit —
  `polkit-gobject-1`

The CLI always builds. The GUI builds only if GTK4 is found; the privileged
D-Bus mechanism builds only if GIO **and** polkit are found. Anything missing is
skipped with a message, and the rest still builds.

**Runtime** (only for the features you use):

- `lsblk` (util-linux) — device enumeration
- `sudo` — CLI auto-elevation for `device` / `format`
- `pkexec` + a polkit agent — GUI elevation
- `sfdisk` (util-linux) — `format --partition`
- `mkfs.exfat` (`exfatprogs`), `mkfs.vfat` (`dosfstools`), … — `--mkfs` / `--partition`
- `udevadm` (systemd) — waits for a new partition node to appear

Linux-only (uses `/sys`, `lsblk`, and block ioctls).

## Download (prebuilt, x86-64)

Prebuilt artifacts are produced by CI (built on Ubuntu 22.04 → glibc 2.35, so
they run on that and **newer** distros) and attached to each
[GitHub Release](https://github.com/frebergguru/sdcard_checker/releases):

- **`SDCardChecker-<ver>-x86_64.AppImage`** — portable GTK4 GUI, zero install.
  `chmod +x` it and run. On distros without libfuse2, run it with
  `./SDCardChecker-*.AppImage --appimage-extract-and-run`. The safe test, device
  listing, and info work fully; destructive operations elevate via a per-use
  password prompt (a self-mounted AppImage can't register the system polkit/D-Bus
  policy — install the archive below for smooth elevation).
- **`sdcheck-<ver>-x86_64.tar.gz`** — prebuilt **system install**: CLI, GUI, the
  root D-Bus mechanism, polkit policy, and desktop launcher/icon. Extract and run
  `sudo ./install.sh`. This is what enables Tier-3 elevation (passwordless for the
  `disk` group / one prompt per session). See the archive's own `README.md`.

To build the AppImage yourself, see `packaging/appimage/build-appimage.sh`; the
CI recipe is `.github/workflows/release.yml`. Or build from source:

## Build

```sh
cmake -B build          # configure — creates build/ and CMakeCache.txt
cmake --build build     # compile
```

`cmake -B build` is the **configure** step; `cmake --build build` only works
after it. Binaries land in `build/sdcheck`, `build/sdcheck-gui`, and (Tier 3)
`build/sdcheck-mechanism`. For later rebuilds just re-run `cmake --build build`.

## Install

One shot — clean, configure, build, install, and reload the system bus:

```sh
./install.sh            # installs to /usr; pass a prefix to override, e.g. ./install.sh /usr/local
```

It builds as you and uses `sudo` only for the install + D-Bus reload (you'll be
prompted). Equivalent manual steps:

```sh
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
sudo systemctl reload dbus        # only needed if the Tier-3 mechanism was built
```

Installed files (under the prefix, `/usr` shown):

| File | Purpose |
|------|---------|
| `bin/sdcheck`, `bin/sdcheck-gui` | CLI and GUI |
| `share/applications/org.sdcheck.gui.desktop` | desktop launcher (GUI) |
| `share/icons/hicolor/scalable/apps/org.sdcheck.gui.svg` | application icon (GUI) |
| `libexec/sdcheck/sdcheck-mechanism` | root D-Bus service (Tier 3) |
| `share/polkit-1/actions/org.sdcheck.policy` | branded auth prompt (Tier 3) |
| `share/polkit-1/rules.d/49-sdcheck.rules` | auth policy (disk group / once-per-session) |
| `share/dbus-1/system.d/org.sdcheck.Mechanism.conf` | system-bus policy (Tier 3) |
| `share/dbus-1/system-services/org.sdcheck.Mechanism.service` | on-demand activation (Tier 3) |

Use `/usr` (not the default `/usr/local`): the system D-Bus daemon only reads
activation files from `/usr/share/dbus-1/...`.

## Uninstall

```sh
sudo cmake --build build --target uninstall
sudo systemctl reload dbus
```

This removes exactly the files recorded in `build/install_manifest.txt`. It
needs the same `build/` directory the install came from. (Empty directories such
as `libexec/sdcheck` are left behind; delete them by hand if you care.)

## CLI usage

```sh
sdcheck list                          # list removable devices + mountpoints
sdcheck info   /dev/mmcblk0           # show card identity (manufacturer, serial, date…)
sdcheck file   /run/media/you/CARD    # SAFE: tests free space, no root, keeps your data
sdcheck device /dev/sdb --yes         # DESTRUCTIVE: tests true capacity (erases card)
sdcheck device /dev/sdb --yes --full  #   ...full surface: write all, read all back, verify
sdcheck device /dev/sdb --yes --full --iops --passes 3 --report r.json   # the works
sdcheck format /dev/sdb --yes         # DESTRUCTIVE: low-level erase
sdcheck format /dev/sdb --yes --partition          # erase, partition, format as exFAT
sdcheck format /dev/sdb --yes --partition --mkfs vfat   # ...or another filesystem
sdcheck auto   /dev/sdb --yes         # DESTRUCTIVE: test, then format as exFAT if genuine
sdcheck selftest                      # internal sanity checks (no hardware touched)
```

Options:

- `device`: `--quick` (default, sparse sampling + size-bounding) | `--full`
  (write the whole surface, read it all back, verify every byte, and build a
  **bad-block map**); `--passes N` repeats the test N times (stress test);
  `--iops` also measures **random 4K IOPS** → SD App class (A1/A2)
- `format`: `--discard` | `--zero` | `--both` (default) erase method;
  `--mkfs TYPE` lay down a filesystem; `--partition` write a partition table and
  `mkfs` the **partition** instead of the bare device (defaults to **exFAT** when
  `--mkfs` is omitted)
- `auto`: runs a `device` test (defaults to `--full`) and, **only if the card is
  genuine**, lays down a fresh partition + filesystem (**exFAT** by default).
  Accepts the `device` test flags plus `--mkfs TYPE`, `--no-partition`, and the
  `--discard`/`--zero`/`--both` erase method (default `--discard`, since the test
  already wrote the surface). A fake/failed card is left unformatted.
- `file` / `device` / `format` / `auto`: `--report FILE` writes a JSON result report

`device`, `format`, and `auto` need root: once you confirm with `--yes`, the CLI
**re-launches itself under `sudo`** automatically, so you don't have to prepend
it yourself.

Exit codes: `0` genuine / limited, `2` fake, `1` error.

### Test modes

- **File test** (`file`) — writes `*.h2w` pattern files into a mounted card's
  free space, then verifies. No root, non-destructive, but only exercises free
  space. Best first check.
- **Device test** (`device`) — writes directly to the block device. **Erases
  everything.** `--quick` bounds the real size fast; `--full` is the thorough
  whole-card write-then-read-back verification.
- **Auto** (`auto`) — a device test followed, **only if the card passes**, by a
  fresh exFAT partition, so a genuine card is verified and made ready to use in
  one step (one privilege prompt). A fake card is left unformatted.

## GUI usage

Launch `sdcheck-gui`. Pick a device, then:

- **Safe file test** — enter/confirm the mountpoint and **Start test**. No root.
  (Destructive controls are disabled in this mode.)
- **Destructive device test** — select the radio, tick **Yes, ERASE…**, and
  **Start test**. Options: **Full surface** (whole-card write+verify + bad-block
  map), **Measure random 4K IOPS** (A1/A2 app class), and **Stress passes** (1–100).
- **Low-level format** — tick **Yes, ERASE…**, pick an **Erase method** and
  **Filesystem**, optionally tick **create a partition table**, then
  **Low-level format**.
- **Test & format** — tick **Yes, ERASE…**, choose the test depth and target
  **Filesystem**, then **Test & format**: the card is tested and, only if it's
  genuine, given a fresh partition (exFAT by default) in a single step.
- **View info** shows the card identity (CID) read-only.
- **Copy log** copies the result panel (also selectable for Ctrl+C); **Save
  report** writes the result as JSON.

For destructive actions the GUI elevates via polkit (see below); enter your
password when prompted. A wrong password or cancel leaves the card untouched and
the window usable. **Cancel** stops a running operation. The CLI and GUI expose
the same set of features.

## How elevation works

The destructive operations need root. The GUI never runs as root itself
(important on Wayland, where a root GUI can't reach the display); instead it
elevates only the small, headless work:

1. **D-Bus mechanism** (preferred, if installed) — the GUI calls a root system
   service that authorizes each request against the polkit action
   `org.sdcheck.manage-device`, giving a branded, per-operation prompt. The
   service is D-Bus-activated on demand and exits when idle.
2. **`pkexec` helper** (fallback) — if the mechanism isn't installed, the GUI
   runs `sdcheck helper …` via `pkexec`.

The installed polkit rule (`49-sdcheck.rules`) lets members of the **`disk`**
group act without a prompt and asks everyone else for an administrator password
**once per session**. Override it by dropping a copy in `/etc/polkit-1/rules.d/`.
Design details: [docs/privilege-separation.md](docs/privilege-separation.md).

## ⚠️ Safety

`device` and `format` are **destructive and irreversible**. As guards they:

- require `--yes` (CLI) / a ticked confirmation (GUI),
- require root,
- **refuse any device that is not removable** (your system disk is protected),
- **unmount** the device (and its partitions) automatically if mounted; if a
  mount is busy and can't be released, the operation is refused.

Always double-check the device node with `sdcheck list` / `lsblk` before running.

## Notes

- Card identity (CID) is only visible in a **native SD slot**; USB card readers
  usually mask it, in which case only the reader's vendor/model is shown.
- `--partition` writes a single whole-disk **MBR** partition typed for the
  filesystem (e.g. `0x07` for exFAT) — the layout cameras and Windows expect.
- **Virtual device for testing**: `device`/`format` accept a regular **image
  file** in place of a block device (it's treated as a virtual card — no root,
  no loop device). Handy for development/CI:
  `truncate -s 256M card.img && sdcheck device ./card.img --yes --full --iops`.
  Real block devices still require removable + root as usual.

## License

This project is licensed under the **GNU General Public License v3.0** (or, at
your option, any later version). See [LICENSE](LICENSE) for the full text.
