# Modules

This folder is reserved for the status module implementation. The shared
interface is available in `include/module.h`.

Planned first modules:

- `clock.c`: update once per second using `timerfd`.
- `battery.c`: read `/sys/class/power_supply/` every few seconds.
- `volume.c`: event-driven through PipeWire or PulseAudio.
- `workspace.c`: subscribe to the compositor workspace protocol (for example
  Sway IPC, or `ext-workspace` when available).

Modules should only update their data when it changes and mark it `dirty`; the
renderer decides when a redraw is needed.
