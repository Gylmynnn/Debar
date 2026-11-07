# debar

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
  libwayland-dev libwayland-bin wayland-protocols libcairo2-dev libpango1.0-dev \
  liblua5.4-dev
```

### Arch Linux

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf \
  wayland wayland-protocols cairo pango lua
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
./build/debar
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
debar/
├── meson.build
├── protocols/
│   └── wlr-layer-shell-unstable-v1.xml
├── include/
│   ├── calendar.h
│   ├── config.h
│   ├── module.h
│   ├── render.h
│   └── wayland.h
└── src/
    ├── main.c
    ├── calendar.c
    ├── config.c
    ├── render.c
    ├── wayland.c
    └── modules/
        └── README.md
```

File XML di `protocols/` berisi subset client-side versi 1 yang dibutuhkan oleh starter ini. Header dan source protokol digenerate otomatis oleh `wayland-scanner` ketika build. Paket `wayland-protocols` menyediakan definisi resmi `xdg-shell` yang diperlukan untuk tipe objek popup.

## Konfigurasi (Lua)

debar membaca `~/.config/debar/config.lua` (atau `$XDG_CONFIG_HOME/debar/config.lua`) saat start. Kalau file tidak ada, nilai default dipakai. Contoh lengkap ada di `examples/config.lua`:

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

- `padding` adalah jarak antara tepi bar dan pill di dalamnya. Tinggi bar mengikuti: `35 (tinggi pill) + top + bottom`, jadi default menghasilkan 53 px seperti sebelumnya.
- `margin` adalah jarak antara bar dan tepi layar. Pada bar `top`, `margin.top` adalah jarak dari tepi atas layar dan `margin.bottom` adalah ruang kosong antara bar dan jendela; pada bar `bottom` sebaliknya. `margin.left` dan `margin.right` mengatur jarak ke tepi kiri/kanan, sehingga bar menyempit sesuai nilainya.
- Opsi yang salah ketik, nilai di luar rentang, atau error sintaks Lua tidak menghentikan bar: pesan ditulis ke stderr dan opsi terkait memakai default.
- `position = "left"` / `"right"` belum didukung (bar vertikal butuh layout sendiri); sementara dipakai `"top"` dengan peringatan.
- Kalender popup muncul di bawah bar jika `position = "top"`, dan di atas bar jika `"bottom"`.

## Kalender popup

Klik kiri pada pill jam untuk membuka kalender mengambang tepat di bawah bar (rata kiri dengan pill jam). Klik lagi pill jam, klik bagian lain bar, tekan `Esc`/`q`, atau pindah fokus ke jendela lain untuk menutupnya.

| Input | Aksi |
| --- | --- |
| `←` / `→` atau scroll mouse di atas popup | bulan sebelumnya / berikutnya |
| `↑` / `↓` | tahun sebelumnya / berikutnya |
| `Enter` | kembali ke hari ini |
| `s` | ganti gaya penanda hari ini (terisi / outline) |

Catatan:

- Popup adalah layer surface kedua (layer `overlay`) dengan `keyboard_interactivity = on_demand`, jadi klik popup sekali agar menerima input keyboard. `on_demand` butuh `wlr-layer-shell` v4, karena itu `protocols/wlr-layer-shell-unstable-v1.xml` dinaikkan ke versi 4 (urutan opcode tidak berubah).
- Tombol keyboard dibaca langsung sebagai kode evdev, sehingga tidak perlu `xkbcommon`.
- Logika tanggal ada di `src/calendar.c` (minggu mulai Senin, selalu 6 baris), penggambaran di `render_calendar()` pada `src/render.c`.

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
