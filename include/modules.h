#ifndef CBAR_MODULES_H
#define CBAR_MODULES_H

#include <stdbool.h>

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

  /* Workspace strip. Static defaults until a compositor protocol is hooked
   * up: ws_active is drawn as the pac-man, bits set in ws_occupied are drawn
   * as filled ghosts, everything else as outlined ghosts. */
  int ws_count;
  int ws_active;
  unsigned ws_occupied;
} modules_data_t;

void modules_init(modules_data_t *data);
void modules_update(modules_data_t *data);

#endif
