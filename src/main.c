#include "config.h"
#include "wayland.h"

#include <stdio.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

int main(void) {
#ifdef __GLIBC__
  /* Keep the resident size small: all threads (the tray runs its own) share
   * one malloc arena instead of each retaining freed memory, and larger
   * blocks go straight back to the OS when freed. */
  mallopt(M_ARENA_MAX, 1);
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
  mallopt(M_TRIM_THRESHOLD, 256 * 1024);
#endif

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
