## Downloads

Two prebuilt **x86-64** artifacts. Built on Ubuntu 24.04, so they need **glibc ≥ 2.39** (any 2024-or-newer distro).

### 🟦 `SDCardChecker-*.AppImage` — portable GUI, no install
The graphical app in a single file. Just make it executable and run:
```sh
chmod +x SDCardChecker-*-x86_64.AppImage
./SDCardChecker-*-x86_64.AppImage
```
On distros without libfuse2, append `--appimage-extract-and-run`.
The safe test, device list, and card info work fully. Destructive operations
(erase / format) still work but ask for your password **each time** — a portable
AppImage can't register the system polkit/D-Bus policy. For smooth elevation,
use the system archive below.

### 📦 `sdcheck-*-x86_64.tar.gz` — full system install
Prebuilt **CLI + GUI + root D-Bus mechanism + polkit policy + desktop launcher/icon**.
Install it when you regularly run the destructive `device` / `format` / `auto`
operations — this enables the proper elevation (passwordless for the `disk`
group, otherwise one prompt per session) and adds the app to your menu.
```sh
tar xf sdcheck-*-x86_64.tar.gz
cd sdcheck-*-x86_64
sudo ./install.sh        # installs to /usr; uninstall with sudo ./uninstall.sh
```

---

**Which one?** Grab the **AppImage** for a quick, no-install GUI. Install the
**tar.gz** for the complete, properly-integrated experience. Full command
reference: see the [project README](https://github.com/frebergguru/sdcard_checker#readme).
