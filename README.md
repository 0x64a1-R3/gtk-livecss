# gtk-livecss

Universal `LD_PRELOAD` runtime CSS hot-reloader for **GTK 3**, **GTK 4**, **Libadwaita**, and GTK-based derivatives
(**Libhandy**, **Libgranite**, `gtk-layer-shell`, **PyGObject**, **GJS**, **Vala**, and **gtk-rs**).

Designed for dynamic wallpaper and color-scheme generators such
as [matugen](https://github.com/InioX/matugen), [wallust](https://codeberg.org/explosion-mental/wallust),
and [pywal](https://github.com/dylanaraps/pywal) across Wayland compositors (**Hyprland**, **Niri**, **Wayfire**,
**Sway**) and X11 environments without compositor-specific environment variables, hardcoded process lists, or restarting
running GTK applications.

## How It Works

1. **Passive Probing & Zero Non-GTK Overhead:** Uses `dlopen(..., RTLD_NOLOAD)` to check whether the host process has
   already mapped GLib/GTK and initialized a default `GdkDisplay` or `GdkScreen`. Non-GLib processes (shells, CLI tools,
   games) exit immediately on a single pointer check without mapping GTK into memory, while headless GLib daemons stop
   polling cleanly after 10 seconds.
2. **`RTLD_LOCAL` & Thread-Safe Interposition:** Resolves interposed symbols with `RTLD_NOLOAD` library handle fallbacks
   when `dlsym(RTLD_NEXT, ...)` returns `NULL`, and restricts attachment to the owner thread of
   `g_main_context_default()`. Runtimes that load GLib/GIO/GTK with `RTLD_LOCAL` or run private worker contexts (such as
   Python/PyGObject, GJS, and OpenJDK/JBR) operate untouched.
3. **High-Priority Live Style Injection:** Registers a `GtkCssProvider` at priority `801`
   (`GTK_STYLE_PROVIDER_PRIORITY_USER + 1`) on the default `GdkDisplay` (GTK 4) or `GdkScreen` (GTK 3) so hot-reloaded
   rules override static startup stylesheets.
4. **Widget Cache & Style Manager Invalidation:** Calls `gtk_style_context_reset_widgets()` for GTK 3 / Libhandy
   applications and emits `notify::gtk-theme-name` and `notify::gtk-application-prefer-dark-theme` on `GtkSettings` so
   style managers (`AdwStyleManager`, `HdyStyleManager`) and theme listeners repaint immediately.
5. **Atomic Write & `@import` Aware:** Watches `~/.config/gtk-3.0/` and `~/.config/gtk-4.0/` via `GFileMonitor` for any
   non-hidden `*.css` change (including atomic temp-file renames of `@import`'d files such as `colors.css`), debounces
   reloads by 50 ms, and suppresses transient mid-write `parsing-error` warnings.

## Installation

### Arch Linux / CachyOS (via `makepkg`)

```bash
git clone https://github.com/0x64a1-R3/gtk-livecss.git
cd gtk-livecss/packaging/arch
makepkg -si
```

Installing the package automatically registers `/usr/lib/libgtk_livecss.so` in `/etc/ld.so.preload` and removes it
cleanly on uninstall:

```bash
# To uninstall
sudo pacman -R gtk-livecss
```

*(For local development builds directly from a dirty working tree, see [
`packaging/arch/dev/README.md`](https://www.google.com/search?q=packaging/arch/dev/README.md).)*

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

*(Alternatively, export `LD_PRELOAD=/usr/lib/libgtk_livecss.so` in your user session if you prefer user-scoped
activation.)*

## Theme Generator Setup (Matugen)

Configure `matugen` (`~/.config/matugen/config.toml`) to output separate CSS files for GTK 3 and GTK 4:

```toml
[templates.gtk3]
input_path = "~/.config/matugen/templates/gtk3-colors.css"
output_path = "~/.config/gtk-3.0/gtk.css"

# GTK 4 and GTK 3 read separate inputs: Libadwaita 1.6+ recolors its widgets
# from CSS custom properties (:root) that GTK 3 cannot parse, so the GTK 4
# template leads with those while GTK 3 keeps @define-color rules.
[templates.gtk4]
input_path = "~/.config/matugen/templates/gtk4-colors.css"
output_path = "~/.config/gtk-4.0/gtk.css"

```

> **Tip for Libadwaita (GTK 4):** Libadwaita widgets style themselves using named CSS variables
> (`@define-color window_bg_color ...;`, `@define-color view_bg_color ...;`, `@define-color headerbar_bg_color ...;`,
> `@define-color accent_bg_color ...;`, etc.). Defining these tokens in your Matugen template allows Libadwaita apps to
> repaint live without restarting or toggling `color-scheme`.

> **Non-GTK Toolkits & Browsers:** `gtk-livecss` intentionally scopes itself strictly to GTK 3, GTK 4, and Libadwaita.
> For Qt (`qt5ct`/`qt6ct`), Chromium-based browsers (`BrowserThemeColor` managed policies + `--refresh-platform-policy`),
> terminals, and bars, use `matugen` templates and `post_hook` commands alongside `gtk-livecss`.

## Verification & Troubleshooting

* **Verify attachment to a running GTK app:**

```bash
grep -q 'gtk_livecss' /proc/$(pidof nautilus)/maps && echo "Attached"
```

* **Already-running processes:** Because `/etc/ld.so.preload` is evaluated when a process starts, GTK applications that
  were already open before installing `gtk-livecss` must be restarted once to pick up live reloading.
* **32-bit applications (Wine / Steam / Proton):** Launching a 32-bit binary prints a harmless message from glibc's
  dynamic linker:

```text
ERROR: ld.so: object '/usr/lib/libgtk_livecss.so' from /etc/ld.so.preload cannot be preloaded (wrong ELF class: ELFCLASS64): ignored.
```

`ld.so` automatically skips the 64-bit shared object and runs the 32-bit application normally.

* **Flatpak applications:** Flatpak sandboxes block access to host `/usr/lib` paths by default. Expose the library and
  GTK config directories read-only:

```bash
flatpak override --user \
  --filesystem=/usr/lib/libgtk_livecss.so:ro \
  --filesystem=xdg-config/gtk-3.0:ro \
  --filesystem=xdg-config/gtk-4.0:ro
```

* **Disabling or Uninstalling:**
* **Arch / CachyOS:** `sudo pacman -R gtk-livecss` (or `gtk-livecss-git` for dev builds)
* **Manual install:** Remove `/usr/lib/libgtk_livecss.so` from `/etc/ld.so.preload` and delete
  `/usr/lib/libgtk_livecss.so`.

## License

This project is licensed under the [MIT License](LICENSE)