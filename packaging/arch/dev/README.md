# Local Arch Development Packaging (`gtk-livecss-git`)

This directory provides a local development `PKGBUILD` for building and installing `gtk-livecss-git` directly from the repository working tree (including uncommitted changes) using `makepkg` and `pacman`.

## Build & Install

From `packaging/arch/dev/`:

```bash
# Build from current working tree and install/upgrade via pacman
makepkg -sif

# Clean build directory first (use after CMakeLists.txt changes or branch switches)
makepkg -Csif
```

If an earlier manual `sudo cmake --install` left an untracked `/usr/lib/libgtk_livecss.so` on disk, let `pacman` take ownership once:

```bash
sudo pacman -U --overwrite '/usr/lib/libgtk_livecss.so' gtk-livecss-git-*.pkg.tar.zst
```

## Differences from Release Packaging

| Feature | `packaging/arch/PKGBUILD` | `packaging/arch/dev/PKGBUILD` |
| --- | --- | --- |
| **Package Name** | `gtk-livecss` | `gtk-livecss-git` (`provides`/`conflicts`: `gtk-livecss`) |
| **Source** | Tagged GitHub release tarball | Local repo root (`$startdir/../../..`) including dirty edits |
| **Version (`pkgver`)** | Static release tag | Auto-generated `<base>.r<commits>.g<sha>` |
| **CMake Build Type** | `None` (Arch `makepkg.conf` flags) | `RelWithDebInfo` (preserves debug symbols for stack traces) |

## `/etc/ld.so.preload` Lifecycle & Removal

`gtk-livecss.install` handles `/etc/ld.so.preload` automatically during `pacman` transactions:

* **Install / Upgrade:** Appends `/usr/lib/libgtk_livecss.so` to `/etc/ld.so.preload` if not already present.
* **Uninstall:** Removes `/usr/lib/libgtk_livecss.so` from `/etc/ld.so.preload` (and deletes `/etc/ld.so.preload` if it becomes empty).

To cleanly unload and uninstall the development package:

```bash
sudo pacman -Rns gtk-livecss-git
```

## Verifying a Dev Build

1. **Confirm preload registration:**
```bash
cat /etc/ld.so.preload
```

2. **Confirm injection into a newly launched GTK3/GTK4 app:**
```bash
grep -q 'gtk_livecss' /proc/$(pidof nautilus)/maps && echo "Attached"
```

3. **Trigger a live stylesheet reload:**
```bash
touch ~/.config/gtk-3.0/gtk.css ~/.config/gtk-4.0/gtk.css
```
