# cbar

Wayland bar ringan berbasis C, `wayland-client`, `wlr-layer-shell`, Cairo, dan Pango.
Project ini adalah fondasi awal: membuat bar di bagian atas layar dan menggambar teks ke buffer shared-memory.

## Fitur saat ini

- Membuka koneksi ke Wayland.
- Membuat `wl_surface` dengan `wlr-layer-shell` pada layer `top`.
- Menempel di sisi atas layar dan meminta exclusive zone sebesar tinggi bar.
- Membuat buffer `wl_shm` menggunakan `memfd_create`.
- Menggambar background dan teks menggunakan Cairo/Pango.
- Menyimpan konfigurasi dasar di `src/config.c`.

## Dependensi

### Debian / Ubuntu

```sh
sudo apt install build-essential meson ninja-build pkg-config \
  libwayland-dev libwayland-bin wayland-protocols libcairo2-dev libpango1.0-dev
```

### Arch Linux

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf \
  wayland wayland-protocols cairo pango
```

`wayland-scanner` harus tersedia di `PATH` (di Debian/Ubuntu disediakan oleh paket `libwayland-bin`).

## Build

File `wlr-layer-shell-unstable-v1.xml` mempertahankan urutan request v1 dan tipe argumen `xdg_popup` sesuai protokol upstream. Jangan menghapus request yang belum dipakai, seperti `set_margin` atau `get_popup`: opcode Wayland ditentukan berdasarkan urutan, sehingga menghapus satu request akan menggeser opcode berikutnya dan menyebabkan protocol error saat runtime. Karena `get_popup` merujuk interface `xdg_popup`, build juga mengambil `xdg-shell.xml` dari paket `wayland-protocols` dan mengompilasi deskriptor interfacenya.


```sh
meson setup build
meson compile -C build
```

## Jalankan

Jalankan dari sesi Wayland yang mendukung `wlr-layer-shell`, misalnya Sway atau Hyprland:

```sh
./build/cbar
```

Untuk menghentikan proses, tekan `Ctrl+C` dari terminal tempat program dijalankan.

## Konfigurasi awal

Default berada di `src/config.c`:

- Tinggi: `32` px
- Padding teks: `12` px
- Font: `Inter 10`
- Background: `#1e1e2e`
- Foreground: `#cdd6f4`

Ubah nilai tersebut lalu build ulang.

## Struktur

```text
cbar/
├── meson.build
├── protocols/
│   └── wlr-layer-shell-unstable-v1.xml
├── include/
│   ├── config.h
│   ├── module.h
│   ├── render.h
│   └── wayland.h
└── src/
    ├── main.c
    ├── config.c
    ├── render.c
    ├── wayland.c
    └── modules/
        └── README.md
```

File XML di `protocols/` berisi subset client-side versi 1 yang dibutuhkan oleh starter ini. Header dan source protokol digenerate otomatis oleh `wayland-scanner` ketika build. Paket `wayland-protocols` menyediakan definisi resmi `xdg-shell` yang diperlukan untuk tipe objek popup.

## Tahap berikutnya

1. Tambahkan sistem modul (`module.h` dan direktori `src/modules/`).
2. Implementasikan clock dengan `timerfd` agar update tidak memakai busy-loop.
3. Tambahkan pembacaan battery dari `/sys/class/power_supply/`.
4. Tambahkan volume melalui PipeWire atau PulseAudio.
5. Tambahkan workspace melalui protokol compositor yang sesuai.
6. Tambahkan config parser dan layout kiri/tengah/kanan.
7. Tambahkan output/monitor selection, tray, serta input click.

## Catatan

- MVP ini menargetkan compositor Wayland yang menyediakan `zwlr_layer_shell_v1`.
- `xkbcommon`, D-Bus, PipeWire, PulseAudio, UPower, dan JSON belum dibutuhkan pada tahap awal.
- Project ini belum mengimplementasikan tray, multi-monitor selection, click action, atau status modules.
