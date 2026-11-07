# debar

Bar Wayland ringan berbasis C dengan `wayland-client`, `wlr-layer-shell`,
Cairo/Pango, dan GDBus. Tampilan terdiri dari pill mengambang: workspace,
jam, system tray dinamis, volume/brightness, CPU, dan RAM.

## Fitur

- Layer surface `top`/`bottom` dengan background transparan dan pill membulat.
- Indikator workspace bergaya Pac-Man: Pac-Man = workspace aktif, ghost penuh =
  workspace terpakai, ghost outline = kosong. Minimal 6 slot selalu ditampilkan;
  slot kosong tetap bisa diklik (compositor yang membuat workspace-nya).
- Klik slot workspace untuk pindah workspace. Mendukung Hyprland dan Sway
  melalui IPC masing-masing.
- Pill jam dengan popup kalender (klik pill jam).
- **System tray dinamis** (`StatusNotifierItem`): ikon muncul/hilang otomatis
  saat aplikasi berjalan/berhenti. Klik kiri = `Activate`, klik tengah =
  `SecondaryActivate`.
- Pill volume + brightness. Scroll mouse di atas pill ini mengubah nilainya;
  scroll di bagian lain bar **tidak** mengganti workspace.
- Pill CPU dan RAM yang dibaca langsung dari `/proc`.
- Konfigurasi Lua untuk posisi bar, padding, dan margin.

## Dependensi

### Debian / Ubuntu

```sh
sudo apt install build-essential meson ninja-build pkg-config \
  libwayland-dev libwayland-bin wayland-protocols libcairo2-dev libpango1.0-dev \
  liblua5.4-dev libglib2.0-dev libgdk-pixbuf-2.0-dev
```

### Arch Linux

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf \
  wayland wayland-protocols cairo pango lua glib2 gdk-pixbuf2
```

`wayland-scanner` harus tersedia di `PATH` (di Debian/Ubuntu disediakan oleh
paket `libwayland-bin`).

- `gio-2.0` (GLib) wajib untuk system tray.
- `gdk-pixbuf-2.0` opsional. Tanpa paket ini tray hanya bisa memuat ikon PNG;
  dengan paket ini (dan loader SVG/librsvg) ikon SVG dari icon theme juga bisa
  dimuat.
- Untuk fitur volume/brightness, sediakan `wpctl` (WirePlumber) atau `pactl`
  (PulseAudio / PipeWire-pulse), serta `brightnessctl`.
- System tray membutuhkan `org.kde.StatusNotifierWatcher` di session bus
  (Waybar, KDE, atau implementasi lain yang menyediakannya).

## Build

```sh
meson setup build
meson compile -C build
```

File `protocols/wlr-layer-shell-unstable-v1.xml` mempertahankan urutan request
v1 dan tipe argumen `xdg_popup` sesuai protokol upstream. Jangan menghapus
request yang belum dipakai, seperti `set_margin` atau `get_popup`: opcode
Wayland ditentukan berdasarkan urutan, sehingga menghapus satu request akan
menggeser opcode berikutnya dan menyebabkan protocol error saat runtime.
Karena `get_popup` merujuk interface `xdg_popup`, build juga mengambil
`xdg-shell.xml` dari paket `wayland-protocols` dan mengompilasi deskriptor
interfacenya.

## Jalankan

Jalankan dari sesi Wayland yang mendukung `wlr-layer-shell`, misalnya Sway atau
Hyprland:

```sh
./build/debar
```

Hentikan dengan `Ctrl+C` dari terminal tempat program dijalankan, atau
`pkill -x debar`.

### Debug

```sh
DEBAR_DEBUG=1 ./build/debar
```

Log detail tray, IPC, dan input ditulis ke stderr. Tanpa variabel ini debar
tetap tenang; hanya peringatan penting (misalnya compositor IPC tidak
terdeteksi) yang tetap dicetak.

`DEBAR_ICON_THEME` dapat dipakai untuk memaksa icon theme yang dipakai tray:

```sh
DEBAR_ICON_THEME=Gruvbox-Plus-Dark ./build/debar
```

Tanpa variabel tersebut, debar membaca `gtk-icon-theme-name` dari
`~/.config/gtk-3.0/settings.ini` atau `gtk-4.0/settings.ini`, lalu jatuh ke
`hicolor`. Rantai pewarisan theme (`Inherits`) ikut diikuti.

## Input

| Area | Aksi |
| --- | --- |
| Slot workspace | Klik kiri: pindah ke workspace tersebut |
| Pill jam | Klik kiri: buka/tutup kalender |
| Ikon tray | Klik kiri: `Activate`; klik tengah: `SecondaryActivate` |
| Pill volume/brightness | Scroll: ubah nilai (5% per notch) |
| Area lain | Scroll diabaikan |

Scroll diakumulasi: mouse wheel mengirim sekitar 10 unit per notch dan touchpad
mengirim pecahan, sehingga scroll cepat tetap terasa proporsional. Perubahan
label ditampilkan langsung (optimistis), lalu diselaraskan dengan pembacaan
`wpctl`/`pactl`/`brightnessctl` berikutnya. Scroll tidak pernah mengganti
workspace; perpindahan workspace sepenuhnya milik keybind compositor.

## Konfigurasi (Lua)

debar membaca `~/.config/debar/config.lua` (atau
`$XDG_CONFIG_HOME/debar/config.lua`) saat start. Kalau file tidak ada, nilai
default dipakai. Contoh ada di `examples/config.lua`:

```sh
mkdir -p ~/.config/debar
cp examples/config.lua ~/.config/debar/config.lua
```

File ini Lua biasa dan harus diakhiri `return { ... }`. Semua opsi opsional.

| Opsi | Nilai | Default |
| --- | --- | --- |
| `position` | `"top"` atau `"bottom"` | `"top"` |
| `padding` | angka (semua sisi) atau tabel `{ top, bottom, left, right }` dalam px | `{ top = 9, bottom = 9, left = 24, right = 24 }` |
| `margin` | sama seperti `padding` | `{ top = 6, bottom = 6, left = 5, right = 5 }` |

- `padding` adalah jarak antara tepi bar dan pill di dalamnya. Tinggi bar
  mengikuti `pill_height + padding.top + padding.bottom`; default menghasilkan
  53 px.
- `margin` adalah jarak antara bar dan tepi layar. Pada bar `top`,
  `margin.top` adalah jarak dari tepi atas layar dan `margin.bottom` adalah
  ruang kosong antara bar dan jendela; pada bar `bottom` sebaliknya.
  `margin.left` dan `margin.right` mengatur jarak ke tepi kiri/kanan, sehingga
  bar menyempit sesuai nilainya.
- Opsi yang salah ketik, nilai di luar rentang, atau error sintaks Lua tidak
  menghentikan bar: pesan ditulis ke stderr dan opsi terkait memakai default.
- `position = "left"` / `"right"` belum didukung; sementara dipakai `"top"`
  dengan peringatan.
- Nilai visual lain (radius, tinggi pill, gap, font, warna) masih berada di
  `src/config.c`. Ubah di sana lalu build ulang.

## Kalender popup

Klik kiri pada pill jam untuk membuka kalender mengambang tepat di bawah bar
(rata kiri dengan pill jam) jika `position = "top"`, dan di atas bar jika
`"bottom"`. Klik lagi pill jam, klik bagian lain bar, tekan `Esc`/`q`, atau
pindah fokus ke jendela lain untuk menutupnya.

| Input | Aksi |
| --- | --- |
| `←` / `→` atau scroll mouse di atas popup | bulan sebelumnya / berikutnya |
| `↑` / `↓` | tahun sebelumnya / berikutnya |
| `Enter` | kembali ke hari ini |
| `s` | ganti gaya penanda hari ini (terisi / outline) |

Catatan:

- Popup adalah layer surface kedua (layer `overlay`) dengan
  `keyboard_interactivity = on_demand`, jadi klik popup sekali agar menerima
  input keyboard. `on_demand` butuh `wlr-layer-shell` v4.
- Tombol keyboard dibaca langsung sebagai kode evdev, sehingga tidak perlu
  `xkbcommon`.
- Logika tanggal ada di `src/calendar.c` (minggu mulai Senin, selalu 6 baris),
  penggambaran di `render_calendar()` pada `src/render.c`.

## System tray

Tray memakai D-Bus `StatusNotifierWatcher`/`StatusNotifierItem` di thread
terpisah dengan `GMainContext` sendiri. Bar tidak pernah memblokir D-Bus:
snapshot ikon dilindungi mutex, dan sebuah self-pipe membangunkan poll loop
Wayland saat isi tray berubah. Aktifasi ikon dijalankan di thread singkat agar
klik tetap responsif.

- Maksimal 12 ikon (`DEBAR_TRAY_MAX_ITEMS`).
- Sumber ikon dipilih berurutan: `IconPixmap` (ARGB32), lalu `IconName` dari
  `IconThemePath` item, lalu icon theme aktif beserta rantai `Inherits`, lalu
  `/usr/share/pixmaps`.
- Item yang tidak punya ikon yang bisa dibaca tetap mendapat slot kosong.
- Jika session bus atau watcher tidak tersedia, tray hanya kosong; bar tetap
  berjalan normal.

## Struktur

```text
debar/
├── meson.build
├── protocols/
│   └── wlr-layer-shell-unstable-v1.xml
├── examples/
│   └── config.lua
├── include/
│   ├── calendar.h
│   ├── config.h
│   ├── ipc.h
│   ├── module.h
│   ├── modules.h
│   ├── render.h
│   ├── tray.h
│   └── wayland.h
└── src/
    ├── main.c
    ├── calendar.c
    ├── config.c
    ├── ipc.c
    ├── modules.c
    ├── render.c
    ├── tray.c
    └── wayland.c
```

Header dan source protokol digenerate otomatis oleh `wayland-scanner` ketika
build. Paket `wayland-protocols` menyediakan definisi resmi `xdg-shell` yang
diperlukan untuk tipe objek popup.

## Catatan

- Target utama adalah compositor Wayland yang menyediakan
  `zwlr_layer_shell_v1` (Hyprland, Sway, dan compositor wlroots lain).
- Workspace dan perpindahan workspace membutuhkan Hyprland atau Sway. Di
  compositor lain bar tetap tampil, tetapi indikator workspace tidak terisi.
- Proyek belum mendukung multi-monitor selection atau penempatan bar per
  output; bar tampil di output yang dipilih compositor secara default.