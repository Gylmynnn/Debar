#ifndef DEBAR_MODULES_H
#define DEBAR_MODULES_H

#include <stdbool.h>
#include <stdint.h>

/* Live data rendered by the bar. Updated once per second. */
typedef struct modules_data {
  char clock[64];
  char cpu[16];
  char mem[16];
  char volume[12];
  char brightness[12];
  char battery[12];
  double battery_fill; /* 0..1, for the icon fill level */
  bool battery_present;

  /* workspace state, maintained by the compositor IPC layer */
  int ws_count;             /* number of open workspaces */
  int ws_active;            /* 0-based index of the focused workspace */
  uint32_t ws_occupied;     /* bitmask of occupied workspaces (bit i = ws i) */
} modules_data_t;

void modules_init(modules_data_t *data);
void modules_update(modules_data_t *data);

/* Wheel actions on the volume/brightness pill. delta_percent is the signed
 * change (e.g. +5 / -10); the external tool runs in the background and the
 * label is moved optimistically right away, then reconciled by the next
 * modules_update(). */
void modules_volume_step(modules_data_t *data, int delta_percent);
void modules_brightness_step(modules_data_t *data, int delta_percent);

/* The bar at least always shows this many workspace slots; empty ones are
 * drawn as outline ghosts and switchable (the compositor creates them). */
#define DEBAR_MIN_WORKSPACES 6

#endif
