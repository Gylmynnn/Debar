#include "config.h"
#include "wayland.h"

#include <stdio.h>

int main(void) {
  bar_config_t config;
  config_load(&config);

  wayland_state_t wayland;
  if (!wayland_init(&wayland, &config)) {
    wayland_finish(&wayland);
    return 1;
  }

  const int result = wayland_run(&wayland);
  wayland_finish(&wayland);
  return result == 0 ? 0 : 1;
}
