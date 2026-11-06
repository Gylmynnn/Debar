#ifndef DEBAR_IPC_H
#define DEBAR_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include "modules.h"

typedef enum ipc_compositor {
  IPC_NONE = 0,
  IPC_HYPRLAND,
  IPC_SWAY,
} ipc_compositor_t;

struct ipc_diag {
  char cmd_socket[64];
  char event_socket[64];
};

/* Detects the running compositor and connects its event stream.
 * Returns the detected backend. */
ipc_compositor_t ipc_init(void);

/* Queries the initial workspace list (count + active id). */
void ipc_query_workspaces(modules_data_t *d);

/* Event stream fd for the poll loop, or -1 when unsupported. */
int ipc_event_fd(void);

/* Consumes pending events from the stream and updates module data. */
void ipc_handle_events(modules_data_t *d);

/* Focus a workspace (1-based). Returns true when the command was sent. */
bool ipc_switch_workspace(modules_data_t *d, int workspace);

void ipc_finish(void);

#endif
