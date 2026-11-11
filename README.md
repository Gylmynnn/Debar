# debar

A lightweight Wayland bar written in C with `wayland-client`, `wlr-layer-shell`,
Cairo/Pango, and GDBus. The UI is a row of floating pills: workspaces, clock,
dynamic system tray, Wi-Fi/Bluetooth, volume/brightness, CPU, and RAM.

<img width="1546" height="69" alt="image" src="https://github.com/user-attachments/assets/47d5f898-a550-405c-8a0a-9a5b9b2ae72b" />

## Features

- `top`/`bottom` layer surface with a transparent background and rounded pills.
- Pac-Man-style workspace indicator: Pac-Man = active workspace, filled ghost =
  occupied workspace, outline ghost = empty. At least 6 slots are always shown;
  empty slots are still clickable (the compositor creates the workspace).
- Click a workspace slot to switch to it. Hyprland and Sway are supported
  through their respective IPC sockets.
- Clock pill with a calendar popup (click the clock pill).
- **Dynamic system tray** (`StatusNotifierItem`): icons appear and disappear
  automatically as applications start and stop. Icons are drawn directly on the
  bar (without a pill), spaced 28 px apart and right-aligned; the volume pill
  and everything to its left shifts with the number of icons. Left click =
  `Activate`, middle click = `SecondaryActivate`.
- **Wi-Fi and Bluetooth manager**: Wi-Fi and Bluetooth icons (Nerd Font glyphs)
  sit just before the volume pill, tinted with their accent colors
  (`COL_WIFI`/`COL_BT` in `src/render.c`). Click an icon to open a
  Windows/macOS-style popup (see below).
- Volume + brightness pill. Scrolling over this pill changes its value;
  scrolling anywhere else on the bar **does not** switch workspaces. Click the
  volume or brightness part to open its slider popup (see below).
- Tray icons belonging to Wi-Fi and Bluetooth applets (nm-applet, blueman, and
  similar) are hidden automatically because the bar has its own popups for
  both.
- CPU and RAM pills read directly from `/proc`. Click a pill to open a detail
  popup with per-core load, temperature, and top programs (see below).
- Lua configuration for bar position, padding, and margins.

## Dependencies

### Debian / Ubuntu

```sh
sudo apt install build-essential meson ninja-build pkg-config \
  libwayland-dev libwayland-bin wayland-protocols libcairo2-dev libpango1.0-dev \
  liblua5.4-dev libglib2.0-dev libgdk-pixbuf-2.0-dev libxkbcommon-dev
```

### Arch Linux

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf \
  wayland wayland-protocols cairo pango lua54 glib2 gdk-pixbuf2 libxkbcommon
```

`wayland-scanner` must be available in `PATH` (on Debian/Ubuntu it is provided
by the `libwayland-bin` package).

- A Nerd Font is required for the Wi-Fi/Bluetooth icons. The bar font
  (`JetBrainsMono Nerd Font` by default) is tried first, then
  `Symbols Nerd Font` as a fallback.
- `xkbcommon` is required (typing the Wi-Fi password in the popup).
- `gio-2.0` (GLib) is required for the system tray.
- `gdk-pixbuf-2.0` is optional. Without it the tray can only load PNG icons;
  with it (and an SVG/librsvg loader) SVG icons from the icon theme are also
  supported.
- For volume/brightness, provide `wpctl` (WirePlumber) or `pactl`
  (PulseAudio / PipeWire-pulse), plus `brightnessctl`.
- Wi-Fi management needs `nmcli` (NetworkManager); Bluetooth management needs
  `bluetoothctl` (BlueZ).
- The system tray needs `org.kde.StatusNotifierWatcher` on the session bus
  (provided by Waybar, KDE, or another implementation).

## Build

```sh
meson setup build
meson compile -C build
```

The file `protocols/wlr-layer-shell-unstable-v1.xml` keeps the v1 request order
and the `xdg_popup` argument type from the upstream protocol. Do not remove
requests that are not used yet, such as `set_margin` or `get_popup`: Wayland
opcodes are assigned by order, so removing one request shifts the following
opcodes and causes a protocol error at runtime. Because `get_popup` references
the `xdg_popup` interface, the build also takes `xdg-shell.xml` from the
`wayland-protocols` package and compiles its interface descriptors.

## Install

### Installer script (curl)

The recommended way to install a prebuilt binary:

```sh
curl -fsSL https://raw.githubusercontent.com/Gylmynnn/Debar/main/install.sh | bash
```

The installer downloads the latest GitHub release, verifies its SHA-256
checksum, and installs `debar` to `~/.local/bin` (no sudo required). Pass
options after `bash -s --`:

```sh
# System-wide install (uses sudo when needed)
curl -fsSL https://raw.githubusercontent.com/Gylmynnn/Debar/main/install.sh | bash -s -- --system

# Custom prefix and/or a specific release tag
curl -fsSL https://raw.githubusercontent.com/Gylmynnn/Debar/main/install.sh | bash -s -- \
  --prefix "$HOME/.local" --version v0.1.0
```

Run `install.sh --help` for all options. Prebuilt binaries are currently
`x86_64` only: the installer automatically picks the Arch Linux build on Arch
(or Arch-based) systems and the Debian/Ubuntu-compatible build elsewhere. On
other architectures, or on distributions with a different Lua SONAME, build
from source. The installer needs `curl`, `tar`, and `install` (coreutils), and
uses `sha256sum` for checksum verification when available.

Uninstall with:

```sh
curl -fsSL https://raw.githubusercontent.com/Gylmynnn/Debar/main/uninstall.sh | bash
```

`--system` removes a system-wide installation, and `--purge` also deletes
`~/.config/debar`:

```sh
curl -fsSL https://raw.githubusercontent.com/Gylmynnn/Debar/main/uninstall.sh | bash -s -- --purge
```

### Makefile

`Makefile` wraps the Meson commands:

```sh
make            # incremental build (runs Meson setup on first use)
make i          # remove build/ and rebuild from scratch
make install    # install to ~/.local/bin/debar
```

`PREFIX` defaults to `~/.local`, so `make install` needs no sudo and the binary
is available as `debar` as long as `~/.local/bin` is in `PATH`. For a
system-wide installation:

```sh
sudo make install PREFIX=/usr/local
```

Remove it with `make uninstall`, or `sudo make uninstall PREFIX=/usr/local` for
a system-wide install. Run `make help` for the target and variable list
(`PREFIX`, `BUILD_DIR`, `DESTDIR`).

## Run

Run it from a Wayland session that supports `wlr-layer-shell`, such as Sway or
Hyprland:

```sh
./build/debar
```

Stop it with `Ctrl+C` in the terminal where it runs, or `pkill -x debar`.

### Debug

```sh
DEBAR_DEBUG=1 ./build/debar
```

Detailed tray, IPC, and input logs go to stderr. If a popup closes by itself,
the log records why (`popup closed: ...`), for example a keyboard focus change,
a click on the bar, or the compositor closing its layer. Without this variable
debar stays quiet; only important warnings (such as no compositor IPC detected)
are printed.

`DEBAR_ICON_THEME` forces the icon theme used by the tray:

```sh
DEBAR_ICON_THEME=Gruvbox-Plus-Dark ./build/debar
```

Without it, debar reads `gtk-icon-theme-name` from
`~/.config/gtk-3.0/settings.ini` or `gtk-4.0/settings.ini`, then falls back to
`hicolor`. The theme inheritance chain (`Inherits`) is followed as well.

## Performance and memory usage

- The bar wakes once per second, and a new frame is only drawn when the data
  changes (the clock only changes once a minute; CPU/RAM/volume labels when
  their values change).
- Wayland buffers are reused (at most two idle buffers), so no new `memfd` +
  `mmap` is created for every frame.
- Volume is read every 2 seconds through `wpctl`/`pactl`, run directly (without
  a shell) with a 300 ms timeout, so a stuck audio server cannot freeze the bar.
  Scrolling on the volume pill reads the value back immediately.
- The battery (not displayed yet) is only read every 30 seconds.
- Tray icons larger than 48 px are downscaled once when loaded (a 512 px pixmap
  goes from 1 MB to 9 KB).
- The glibc heap is tuned to stay small: one arena for all threads and memory
  from icon lookups is returned to the OS (`malloc_trim`).

## Configuration (Lua)

debar reads `~/.config/debar/config.lua` (or
`$XDG_CONFIG_HOME/debar/config.lua`) at startup. If the file does not exist,
the defaults are used. An example is in `examples/config.lua`:

```sh
mkdir -p ~/.config/debar
cp examples/config.lua ~/.config/debar/config.lua
```

The file is plain Lua and must end with `return { ... }`. Every option is
optional.

| Option | Value | Default |
| --- | --- | --- |
| `position` | `"top"` or `"bottom"` | `"top"` |
| `padding` | a number (all sides) or a table `{ top, bottom, left, right }` in px | `{ top = 9, bottom = 9, left = 24, right = 24 }` |
| `margin` | same as `padding` | `{ top = 6, bottom = 6, left = 5, right = 5 }` |

- `padding` is the space between the bar edges and the pills inside it. The bar
  height follows `pill_height + padding.top + padding.bottom`; the default
  produces 53 px.
- `margin` is the space between the bar and the screen edges. For a `top` bar,
  `margin.top` is the distance from the top edge and `margin.bottom` is the
  empty space kept between the bar and windows; for a `bottom` bar it is the
  other way around. `margin.left` and `margin.right` control the distance to the
  left/right edges, so the bar narrows accordingly.
- Typos, out-of-range values, or Lua syntax errors do not stop the bar: the
  message is written to stderr and the affected option keeps its default.
- `position = "left"` / `"right"` is not supported yet; `"top"` is used with a
  warning.
- Other visual values (radius, pill height, gap, font, colors) still live in
  `src/config.c`. Change them there and rebuild.

## Notes

- The main target is a Wayland compositor that provides `zwlr_layer_shell_v1`
  (Hyprland, Sway, and other wlroots compositors).
- Workspaces and workspace switching require Hyprland or Sway. On other
  compositors the bar still renders, but the workspace indicator is not
  populated.
- The project does not support multi-monitor selection or per-output bar
  placement yet; the bar appears on the output chosen by the compositor.
