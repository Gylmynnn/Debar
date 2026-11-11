#ifndef DEBAR_WAYLAND_H
#define DEBAR_WAYLAND_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "calendar.h"
#include "config.h"
#include "render.h"
#include "wlr-layer-shell-client-protocol.h"

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

/* Which popup (if any) is on screen. Only one at a time. */
typedef enum popup_kind {
  POPUP_NONE = 0,
  POPUP_CALENDAR,
  POPUP_WIFI,
  POPUP_BLUETOOTH,
  POPUP_CPU,
  POPUP_MEM,
  POPUP_VOLUME,
  POPUP_BRIGHTNESS,
} popup_kind_t;

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
  tray_t *tray;
  const bar_config_t *config;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_keyboard *keyboard;
  struct wl_surface *pointer_surface;  /* surface under the pointer */
  struct wl_surface *keyboard_surface; /* surface with keyboard focus */
  uint32_t layer_shell_version;
  double pointer_x;
  double pointer_y;

  /* floating popup (calendar, managers, CPU/RAM); created on demand, see
   * popup_open */
  struct wl_surface *popup_surface;
  struct zwlr_layer_surface_v1 *popup_layer;
  bar_buffer_t *popup_buffers;
  bar_buffer_t *popup_active;
  bool popup_configured;
  popup_kind_t popup_kind;
  int popup_w;
  int popup_h;
  calendar_t calendar;

  /* volume / brightness popups */
  levelpopup_ui_t levelui;

  /* Wi-Fi / Bluetooth manager popups */
  netpopup_ui_t netui;
  char password[64]; /* typed Wi-Fi password, wiped after use */

  /* Typing needs a keymap, but only while the password prompt is open: the
   * compositor's keymap fd is kept (costs nothing) and compiled on demand. */
  int keymap_fd;
  uint32_t keymap_size;
  uint32_t mods_depressed;
  uint32_t mods_latched;
  uint32_t mods_locked;
  uint32_t mods_group;
  struct xkb_context *xkb_context;
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;

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
