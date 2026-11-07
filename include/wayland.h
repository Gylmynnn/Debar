#ifndef DEBAR_WAYLAND_H
#define DEBAR_WAYLAND_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "calendar.h"
#include "config.h"
#include "render.h"
#include "wlr-layer-shell-client-protocol.h"

typedef struct wayland_state {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  bar_buffer_t *buffers;
  bar_buffer_t *active_buffer;
  const bar_config_t *config;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_keyboard *keyboard;
  struct wl_surface *pointer_surface;  /* surface under the pointer */
  struct wl_surface *keyboard_surface; /* surface with keyboard focus */
  uint32_t layer_shell_version;
  double pointer_x;
  double pointer_y;

  /* floating calendar popup (created on demand, see popup_open) */
  struct wl_surface *popup_surface;
  struct zwlr_layer_surface_v1 *popup_layer;
  bar_buffer_t *popup_buffers;
  bar_buffer_t *popup_active;
  bool popup_configured;
  calendar_t calendar;

  bool running;
  bool configured;
  int width;
  int height;
} wayland_state_t;

bool wayland_init(wayland_state_t *state, const bar_config_t *config);
int wayland_run(wayland_state_t *state);
void wayland_redraw(wayland_state_t *state);
void wayland_finish(wayland_state_t *state);

#endif
