# Modules

Folder ini disiapkan untuk implementasi modul status. Interface bersama tersedia di `include/module.h`.

Rencana modul pertama:

- `clock.c`: update memakai `timerfd` setiap satu detik.
- `battery.c`: baca `/sys/class/power_supply/` dengan interval beberapa detik.
- `volume.c`: event-driven lewat PipeWire atau PulseAudio.
- `workspace.c`: subscribe ke protokol workspace compositor (misalnya Sway IPC atau ext-workspace bila tersedia).

Modul sebaiknya hanya memperbarui data saat berubah dan menandai `dirty`; renderer yang menentukan kapan perlu redraw.
