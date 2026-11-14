# gtk-livecss

Universal `LD_PRELOAD` runtime CSS hot-reloader for **GTK 3**, **GTK 4**, **Libadwaita**, **Libhandy**, and **Python/PyGObject** applications.

Designed for dynamic wallpaper and color-scheme generators such as [matugen](https://github.com/InioX/matugen), [wallust](https://codeberg.org/explosion-mental/wallust), and [pywal](https://github.com/dylanaraps/pywal) across Wayland compositors (**Hyprland**, **Niri**, **Wayfire**, **Sway**) without needing compositor-specific environment variables or restarting running applications.

## How It Works

1. **Zero overhead in non-GTK processes:** Uses `dlopen(..., RTLD_NOLOAD)` to passively check whether the host process has already mapped `libgtk-4.so.1` or `libgtk-3.so.0`. Shells, CLI tools, and games exit immediately with a single pointer check without loading GTK into memory.
2. **Cross-toolkit & language support:** Works natively with C/C++, Rust (`gtk4-rs`), Vala, GJS, and Python (`PyGObject`) without `GType` registry collisions or `RTLD_LOCAL` symbol resolution issues.
3. **High-priority live injection:** Registers a `GtkCssProvider` at priority `801` (`GTK_STYLE_PROVIDER_PRIORITY_USER + 1`) on the default `GdkDisplay` (GTK 4) or `GdkScreen` (GTK 3) so hot-reloaded rules override initial startup styles.
4. **Atomic write & `@import` aware:** Attaches a `GFileMonitor` to `~/.config/gtk-3.0/` and `~/.config/gtk-4.0/`, matching all non-hidden `*.css` updates (such as `colors.css` imported via `@import 'colors.css';`) and reloading cleanly after a 50 ms debounce window.

## Installation

### Arch Linux / CachyOS (via `makepkg`)

```bash
git clone https://github.com/0x64a1-R3/gtk-livecss.git
cd gtk-livecss/packaging/arch
makepkg -si
```

Installing the package automatically registers `/usr/lib/libgtk_livecss.so` in `/etc/ld.so.preload` and cleanly removes it when uninstalled.

```bash
# To uninstall
sudo pacman -R gtk-livecss
```

### Manual CMake Build (Any Linux Distribution)

#### Dependencies
* CMake >= 3.25
* C++20 compiler (`gcc` or `clang`)
* `glibc`

#### Build & Install

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build
sudo cmake --install build
```

Enable system-wide preloading via `/etc/ld.so.preload`:

```bash
echo '/usr/lib/libgtk_livecss.so' | sudo tee -a /etc/ld.so.preload
```

*(Alternatively, export `LD_PRELOAD=/usr/lib/libgtk_livecss.so` in your user environment if you prefer user-scoped activation.)*

## Theme Generator Setup (Matugen)

Configure `matugen` (`~/.config/matugen/config.toml`) to output `css` for both GTK 3 and GTK 4:

```toml
[templates.gtk3]
input_path = "~/.config/matugen/templates/gtk3-colors.css"
output_path = "~/.config/gtk-3.0/gtk.css"

# GTK 4 and GTK 3 read separate inputs: libadwaita 1.6+ recolours its widgets
# from CSS custom properties (:root) that GTK 3 cannot parse, so the GTK 4
# template leads with those and GTK 3 keeps the @define-color form it can read.
[templates.gtk4]
input_path = "~/.config/matugen/templates/gtk4-colors.css"
output_path = "~/.config/gtk-4.0/gtk.css"
```

> **Tip for Libadwaita (GTK 4):** Libadwaita widgets style themselves using named CSS variables (`@define-color window_bg_color ...;`, `@define-color view_bg_color ...;`, `@define-color headerbar_bg_color ...;`, `@define-color accent_bg_color ...;`, etc.). Defining these tokens in your Matugen template allows Libadwaita apps to repaint live without restarting or toggling `color-scheme`.

## Notes & Troubleshooting

* **32-bit applications (Wine / Steam / Proton):** Launching a 32-bit binary prints a harmless message from glibc's dynamic linker:
  ```text
  ERROR: ld.so: object '/usr/lib/libgtk_livecss.so' from /etc/ld.so.preload cannot be preloaded (wrong ELF class: ELFCLASS64): ignored.
  ```
  `ld.so` automatically skips the 64-bit shared object and runs the 32-bit application normally.
* **Flatpak applications:** Flatpak sandboxes block access to host `/usr/lib` paths by default. Expose the library and GTK config directories read-only:
  ```bash
  flatpak override --user \
    --filesystem=/usr/lib/libgtk_livecss.so:ro \
    --filesystem=xdg-config/gtk-3.0:ro \
    --filesystem=xdg-config/gtk-4.0:ro
  ```
* **Disabling or Uninstalling:**
  * **Arch / CachyOS:** `sudo pacman -R gtk-livecss`
  * **Manual install:** Remove `/usr/lib/libgtk_livecss.so` from `/etc/ld.so.preload` and delete `/usr/lib/libgtk_livecss.so`.

## License

This project is licensed under the [MIT License](LICENSE).