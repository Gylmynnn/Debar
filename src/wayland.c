#include "wayland.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"

static modules_data_t wayland_data; /* live data, updated every second */

static int g_debug = -1;
static void dlog(const char *fmt, ...) {
  va_list ap;
  if (g_debug < 0) {
    g_debug = getenv("DEBAR_DEBUG") != NULL;
  }
  if (g_debug) {
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
  }
}

/* ---------------- calendar popup ----------------------------------------- */

/* evdev key codes (wl_keyboard.key reports raw evdev codes), so no xkbcommon
 * dependency is needed for these few fixed keys. */
#define EVDEV_KEY_ESC 1
#define EVDEV_KEY_Q 16
#define EVDEV_KEY_ENTER 28
#define EVDEV_KEY_S 31
#define EVDEV_KEY_KPENTER 96
#define EVDEV_KEY_UP 103
#define EVDEV_KEY_LEFT 105
#define EVDEV_KEY_RIGHT 106
#define EVDEV_KEY_DOWN 108
#define EVDEV_BTN_LEFT 0x110

/* gap between the bar and the calendar popup, in px */
#define CAL_BAR_GAP 6

static void reap_list(bar_buffer_t **head) {
  bar_buffer_t **link = head;
  while (*link != NULL) {
    bar_buffer_t *buffer = *link;
    if (buffer->retired && !buffer->busy) {
      *link = buffer->next;
      render_buffer_destroy(buffer);
    } else {
      link = &buffer->next;
    }
  }
}

static void popup_redraw(wayland_state_t *state) {
  if (state->popup_surface == NULL || !state->popup_configured) {
    return;
  }

  bar_buffer_t *buffer =
      render_buffer_create(state->shm, CAL_WIDTH, CAL_HEIGHT);
  if (buffer == NULL) {
    fprintf(stderr, "debar: failed to create calendar buffer\n");
    return;
  }

  render_calendar(buffer, state->config, &state->calendar);

  if (state->popup_active != NULL) {
    state->popup_active->retired = true;
  }
  buffer->busy = true;
  buffer->next = state->popup_buffers;
  state->popup_buffers = buffer;
  state->popup_active = buffer;

  wl_surface_attach(state->popup_surface, buffer->wl_buffer, 0, 0);
  wl_surface_damage(state->popup_surface, 0, 0, CAL_WIDTH, CAL_HEIGHT);
  wl_surface_commit(state->popup_surface);
  wl_display_flush(state->display);
}

static void popup_close(wayland_state_t *state) {
  if (state->popup_surface == NULL) {
    return;
  }

  if (state->pointer_surface == state->popup_surface) {
    state->pointer_surface = NULL;
  }
  if (state->keyboard_surface == state->popup_surface) {
    state->keyboard_surface = NULL;
  }

  if (state->popup_layer != NULL) {
    zwlr_layer_surface_v1_destroy(state->popup_layer);
    state->popup_layer = NULL;
  }
  wl_surface_destroy(state->popup_surface);
  state->popup_surface = NULL;
  state->popup_configured = false;

  bar_buffer_t *buffer = state->popup_buffers;
  while (buffer != NULL) {
    bar_buffer_t *next = buffer->next;
    render_buffer_destroy(buffer);
    buffer = next;
  }
  state->popup_buffers = NULL;
  state->popup_active = NULL;

  wl_display_flush(state->display);
}

static void popup_layer_configure(void *data,
                                  struct zwlr_layer_surface_v1 *layer_surface,
                                  uint32_t serial, uint32_t width,
                                  uint32_t height) {
  (void)width;
  (void)height;
  wayland_state_t *state = data;
  zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
  state->popup_configured = true;
  popup_redraw(state);
}

static void popup_layer_closed(void *data,
                               struct zwlr_layer_surface_v1 *layer_surface) {
  (void)layer_surface;
  popup_close((wayland_state_t *)data);
}

static const struct zwlr_layer_surface_v1_listener popup_layer_listener = {
    .configure = popup_layer_configure,
    .closed = popup_layer_closed,
};

/* Opens the calendar just below the bar, left-aligned with the clock pill. */
static void popup_open(wayland_state_t *state) {
  if (state->popup_surface != NULL || !state->configured) {
    return;
  }

  const bar_config_t *config = state->config;
  calendar_goto_today(&state->calendar);

  double clock_x = 0.0;
  double clock_w = 0.0;
  render_clock_span(config, &wayland_data, &clock_x, &clock_w);

  const int screen_w =
      state->width + config->margin.left + config->margin.right;
  int left = config->margin.left + (int)clock_x;
  if (left + CAL_WIDTH > screen_w - config->margin.right) {
    left = screen_w - config->margin.right - CAL_WIDTH;
  }
  if (left < config->margin.left) {
    left = config->margin.left;
  }
  /* screen edge -> bar -> popup */
  const bool at_bottom = config->position == BAR_POSITION_BOTTOM;
  const int offset = (at_bottom ? config->margin.bottom : config->margin.top) +
                     state->height + CAL_BAR_GAP;

  state->popup_surface = wl_compositor_create_surface(state->compositor);
  if (state->popup_surface == NULL) {
    fprintf(stderr, "debar: cannot create calendar surface\n");
    return;
  }

  state->popup_layer = zwlr_layer_shell_v1_get_layer_surface(
      state->layer_shell, state->popup_surface, NULL,
      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "debar-calendar");
  if (state->popup_layer == NULL) {
    fprintf(stderr, "debar: cannot create calendar layer surface\n");
    wl_surface_destroy(state->popup_surface);
    state->popup_surface = NULL;
    return;
  }

  zwlr_layer_surface_v1_set_size(state->popup_layer, CAL_WIDTH, CAL_HEIGHT);
  zwlr_layer_surface_v1_set_anchor(
      state->popup_layer,
      (at_bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                 : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) |
          ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
  zwlr_layer_surface_v1_set_margin(state->popup_layer, at_bottom ? 0 : offset,
                                   0, at_bottom ? offset : 0, left);
  /* -1: position from the screen edge, ignore the bar's exclusive zone */
  zwlr_layer_surface_v1_set_exclusive_zone(state->popup_layer, -1);
  /* on_demand (layer-shell v4): keyboard focus after the first click on the
   * popup, and released again when the user clicks elsewhere. */
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      state->popup_layer,
      state->layer_shell_version >= 4
          ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
          : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  zwlr_layer_surface_v1_add_listener(state->popup_layer, &popup_layer_listener,
                                     state);

  /* The first commit must not contain a buffer. */
  wl_surface_commit(state->popup_surface);
  wl_display_flush(state->display);
}

static void popup_toggle(wayland_state_t *state) {
  if (state->popup_surface != NULL) {
    popup_close(state);
  } else {
    popup_open(state);
  }
}

static void popup_handle_key(wayland_state_t *state, uint32_t key) {
  switch (key) {
  case EVDEV_KEY_ESC:
  case EVDEV_KEY_Q:
    popup_close(state);
    return;
  case EVDEV_KEY_LEFT:
    calendar_shift_month(&state->calendar, -1);
    break;
  case EVDEV_KEY_RIGHT:
    calendar_shift_month(&state->calendar, 1);
    break;
  case EVDEV_KEY_UP:
    calendar_shift_year(&state->calendar, -1);
    break;
  case EVDEV_KEY_DOWN:
    calendar_shift_year(&state->calendar, 1);
    break;
  case EVDEV_KEY_ENTER:
  case EVDEV_KEY_KPENTER:
    calendar_goto_today(&state->calendar);
    break;
  case EVDEV_KEY_S:
    calendar_toggle_style(&state->calendar);
    break;
  default:
    return;
  }
  popup_redraw(state);
}

/* Mouse wheel over the popup flips months. */
static void popup_handle_axis(wayland_state_t *state, double value) {
  static struct timespec last = {0, 0};

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  const long now_ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;
  const long last_ms = last.tv_sec * 1000 + last.tv_nsec / 1000000;
  if (last_ms != 0 && now_ms - last_ms < 120) {
    return;
  }
  last = now;

  calendar_shift_month(&state->calendar, value > 0.0 ? 1 : -1);
  popup_redraw(state);
}

/* ---------------- pointer input ------------------------------------------ */

static void pointer_handle_button(wayland_state_t *state, uint32_t button) {
  /* A click inside the popup only gives it keyboard focus. */
  if (state->popup_surface != NULL &&
      state->pointer_surface == state->popup_surface) {
    return;
  }

  /* Left click on the clock pill toggles the calendar. */
  double clock_x = 0.0;
  double clock_w = 0.0;
  if (button == EVDEV_BTN_LEFT &&
      render_clock_span(state->config, &wayland_data, &clock_x, &clock_w) &&
      state->pointer_x >= clock_x && state->pointer_x <= clock_x + clock_w) {
    popup_toggle(state);
    return;
  }

  /* Any other click on the bar dismisses an open calendar. */
  popup_close(state);

  int index = -1;
  const bool hit = render_workspace_hit(state->config, state->height,
                                        &wayland_data, state->pointer_x,
                                        state->pointer_y, &index);
  dlog("debar debug: click at (%.1f, %.1f) -> hit=%d index=%d",
       state->pointer_x, state->pointer_y, hit ? 1 : 0, index);
  if (!hit) {
    return;
  }
  if (ipc_switch_workspace(&wayland_data, index + 1)) {
    dlog("debar debug: switched to workspace %d", index + 1);
    wayland_redraw(state);
  } else {
    dlog("debar debug: ipc_switch_workspace FAILED");
  }
}

/* Wheel input is accumulated: a mouse wheel sends ~10 axis units per notch,
 * touchpads send a stream of fractions. Whole notches become 5% steps, so
 * fast scrolling applies proportionally instead of dropping events. */
#define WHEEL_UNITS_PER_NOTCH 10.0
#define WHEEL_STEP_PERCENT 5
#define WHEEL_MAX_NOTCHES 5

/* The label moves instantly; give the external tool a moment, then reconcile
 * with the authoritative value from the next poll. */
#define ACTION_REFRESH_DELAY_MS 120
static long g_refresh_at_ms = 0;
static double g_wheel_acc = 0.0;

static long mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void pointer_handle_axis(wayland_state_t *state, double value) {
  /* The wheel never switches workspaces - that belongs to the compositor's
   * keybinds. It only acts on the volume/brightness pill. */
  const render_scroll_target_t target = render_scroll_hit(
      state->config, &wayland_data, (double)state->width, state->pointer_x,
      state->pointer_y);
  if (target == RENDER_SCROLL_NONE) {
    g_wheel_acc = 0.0; /* leaving the pill forgets partial notches */
    return;
  }

  g_wheel_acc += value;
  int notches = 0;
  while (g_wheel_acc >= WHEEL_UNITS_PER_NOTCH) {
    notches++;
    g_wheel_acc -= WHEEL_UNITS_PER_NOTCH;
  }
  while (g_wheel_acc <= -WHEEL_UNITS_PER_NOTCH) {
    notches--;
    g_wheel_acc += WHEEL_UNITS_PER_NOTCH;
  }
  if (notches == 0) {
    return;
  }
  if (notches > WHEEL_MAX_NOTCHES) {
    notches = WHEEL_MAX_NOTCHES;
  } else if (notches < -WHEEL_MAX_NOTCHES) {
    notches = -WHEEL_MAX_NOTCHES;
  }

  /* wheel down (positive axis) lowers the value */
  const int delta = -notches * WHEEL_STEP_PERCENT;
  if (target == RENDER_SCROLL_VOLUME) {
    modules_volume_step(&wayland_data, delta);
  } else {
    modules_brightness_step(&wayland_data, delta);
  }

  /* the step functions already moved the label: show it right away */
  wayland_redraw(state);
  g_refresh_at_ms = mono_ms() + ACTION_REFRESH_DELAY_MS;
  dlog("debar debug: wheel over %s, %+d%%",
       target == RENDER_SCROLL_VOLUME ? "volume" : "brightness", delta);
}


/* ---------------- Seat and pointer listeners ----------------------------- */

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t surface_x, wl_fixed_t surface_y) {
  (void)serial;
  (void)pointer;
  wayland_state_t *state = data;
  state->pointer_surface = surface;
  state->pointer_x = wl_fixed_to_double(surface_x);
  state->pointer_y = wl_fixed_to_double(surface_y);
  dlog("debar debug: pointer enter (%.1f, %.1f)", state->pointer_x,
       state->pointer_y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
  (void)serial;
  (void)pointer;
  wayland_state_t *state = data;
  if (state->pointer_surface == surface) {
    state->pointer_surface = NULL;
  }
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t surface_x,
                           wl_fixed_t surface_y) {
  (void)time;
  (void)pointer;
  wayland_state_t *state = data;
  state->pointer_x = wl_fixed_to_double(surface_x);
  state->pointer_y = wl_fixed_to_double(surface_y);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t button_state) {
  (void)pointer;
  (void)serial;
  (void)time;
  /* any button switches the workspace under the pointer; the calendar
   * itself only reacts to the left button */
  if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
    pointer_handle_button((wayland_state_t *)data, button);
  }
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis,
                         wl_fixed_t value) {
  (void)pointer;
  (void)time;
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    return;
  }
  wayland_state_t *state = data;
  if (state->popup_surface != NULL &&
      state->pointer_surface == state->popup_surface) {
    popup_handle_axis(state, wl_fixed_to_double(value));
  } else {
    pointer_handle_axis(state, wl_fixed_to_double(value));
  }
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
  (void)data;
  (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t axis_source) {
  (void)data;
  (void)pointer;
  (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis) {
  (void)data;
  (void)pointer;
  (void)time;
  (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete) {
  (void)data;
  (void)pointer;
  (void)axis;
  (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

/* ---------------- keyboard (calendar shortcuts) -------------------------- */

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size) {
  (void)data;
  (void)keyboard;
  (void)format;
  (void)size;
  close(fd); /* raw evdev codes are enough, the keymap is not parsed */
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
  (void)keyboard;
  (void)serial;
  (void)keys;
  wayland_state_t *state = data;
  state->keyboard_surface = surface;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface) {
  (void)keyboard;
  (void)serial;
  wayland_state_t *state = data;
  if (state->keyboard_surface == surface) {
    state->keyboard_surface = NULL;
  }
  /* focus moved to another window: dismiss the calendar */
  if (state->popup_surface != NULL && surface == state->popup_surface) {
    popup_close(state);
  }
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t key_state) {
  (void)keyboard;
  (void)serial;
  (void)time;
  wayland_state_t *state = data;
  if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED &&
      state->popup_surface != NULL &&
      state->keyboard_surface == state->popup_surface) {
    popup_handle_key(state, key);
  }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group) {
  (void)data;
  (void)keyboard;
  (void)serial;
  (void)mods_depressed;
  (void)mods_latched;
  (void)mods_locked;
  (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
  (void)data;
  (void)keyboard;
  (void)rate;
  (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities) {
  wayland_state_t *state = data;
  dlog("debar debug: seat capabilities=0x%x", capabilities);
  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0) {
    state->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(state->pointer, &pointer_listener, state);
    dlog("debar debug: pointer created");
  }
  if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 &&
      state->keyboard == NULL) {
    state->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
    dlog("debar debug: keyboard created");
  }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
  (void)data;
  (void)seat;
  (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  wayland_state_t *state = data;

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    const uint32_t bind_version = version < 4 ? version : 4;
    state->compositor = wl_registry_bind(
        registry, name, &wl_compositor_interface, bind_version);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    /* v4 adds keyboard_interactivity=on_demand, used by the calendar. */
    const uint32_t bind_version = version < 4 ? version : 4;
    state->layer_shell = wl_registry_bind(
        registry, name, &zwlr_layer_shell_v1_interface, bind_version);
    state->layer_shell_version = bind_version;
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    const uint32_t bind_version = version < 7 ? version : 7;
    state->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                   bind_version);
    /* Attach immediately: the capabilities event is emitted by the bind
     * during the registry roundtrip, and a listener added later would
     * already have missed it. */
    wl_seat_add_listener(state->seat, &seat_listener, state);
  }
}

static void registry_remove(void *data, struct wl_registry *registry,
                            uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *layer_surface,
                                    uint32_t serial, uint32_t width,
                                    uint32_t height) {
  wayland_state_t *state = data;
  zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

  state->width = width > 0 ? (int)width : 1280;
  state->height = height > 0 ? (int)height : state->config->height;
  state->configured = true;

  wayland_redraw(state);
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *layer_surface) {
  (void)layer_surface;
  wayland_state_t *state = data;
  state->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

bool wayland_init(wayland_state_t *state, const bar_config_t *config) {
  if (state == NULL || config == NULL) {
    return false;
  }

  memset(state, 0, sizeof(*state));
  state->config = config;
  state->running = true;
  calendar_init(&state->calendar);
  state->display = wl_display_connect(NULL);
  if (state->display == NULL) {
    fprintf(stderr, "debar: cannot connect to Wayland display\n");
    return false;
  }

  state->registry = wl_display_get_registry(state->display);
  if (state->registry == NULL) {
    fprintf(stderr, "debar: cannot get Wayland registry\n");
    return false;
  }

  wl_registry_add_listener(state->registry, &registry_listener, state);
  if (wl_display_roundtrip(state->display) < 0) {
    fprintf(stderr, "debar: registry roundtrip failed\n");
    return false;
  }

  if (state->compositor == NULL || state->shm == NULL ||
      state->layer_shell == NULL) {
    fprintf(stderr,
            "debar: compositor, wl_shm, or wlr-layer-shell is unavailable\n");
    return false;
  }

  state->surface = wl_compositor_create_surface(state->compositor);
  if (state->surface == NULL) {
    fprintf(stderr, "debar: cannot create wl_surface\n");
    return false;
  }

  state->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      state->layer_shell, state->surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
      "debar");
  if (state->layer_surface == NULL) {
    fprintf(stderr, "debar: cannot create layer surface\n");
    return false;
  }

  zwlr_layer_surface_v1_set_size(state->layer_surface, 0,
                                 (uint32_t)config->height);
  const bool at_bottom = config->position == BAR_POSITION_BOTTOM;
  zwlr_layer_surface_v1_set_anchor(
      state->layer_surface,
      (at_bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                 : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) |
          ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
          ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  /* Floating bar: margins leave the wallpaper visible around it. Only the
   * margin on the anchored edge counts; the opposite one becomes extra
   * exclusive space so windows keep that distance from the bar. */
  const int outer = at_bottom ? config->margin.bottom : config->margin.top;
  const int inner = at_bottom ? config->margin.top : config->margin.bottom;
  zwlr_layer_surface_v1_set_margin(
      state->layer_surface, at_bottom ? 0 : outer, config->margin.right,
      at_bottom ? outer : 0, config->margin.left);
  /* The compositor adds the anchored-edge margin to the exclusive zone
   * itself, so reserve only the bar plus the inner gap. */
  zwlr_layer_surface_v1_set_exclusive_zone(state->layer_surface,
                                           config->height + inner);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      state->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  zwlr_layer_surface_v1_add_listener(state->layer_surface,
                                     &layer_surface_listener, state);

  /* The first commit must not contain a buffer. */
  wl_surface_commit(state->surface);
  wl_display_flush(state->display);

  modules_init(&wayland_data);
  ipc_init();
  ipc_query_workspaces(&wayland_data);
  return true;
}

/* Renders one frame from the current live data and queues the previous
 * buffer for reaping once the compositor releases it. */
void wayland_redraw(wayland_state_t *state) {
  if (state == NULL || !state->configured || state->width <= 0) {
    return;
  }

  bar_buffer_t *buffer =
      render_buffer_create(state->shm, state->width, state->height);
  if (buffer == NULL) {
    fprintf(stderr, "debar: failed to create Wayland buffer\n");
    return;
  }

  render_bar(buffer, state->config, &wayland_data);

  if (state->active_buffer != NULL) {
    state->active_buffer->retired = true;
  }
  buffer->busy = true;
  buffer->next = state->buffers;
  state->buffers = buffer;
  state->active_buffer = buffer;

  wl_surface_attach(state->surface, buffer->wl_buffer, 0, 0);
  wl_surface_damage(state->surface, 0, 0, state->width, state->height);
  wl_surface_commit(state->surface);
  wl_display_flush(state->display);
}

static void reap_retired_buffers(wayland_state_t *state) {
  reap_list(&state->buffers);
  reap_list(&state->popup_buffers);
}

int wayland_run(wayland_state_t *state) {
  if (state == NULL || state->display == NULL) {
    return -1;
  }

  const int display_fd = wl_display_get_fd(state->display);
  if (display_fd < 0) {
    fprintf(stderr, "debar: cannot get display fd\n");
    return -1;
  }

  const int ipc_fd = ipc_event_fd();
  struct pollfd fds[2];
  struct pollfd *display_poll = &fds[0];
  display_poll->fd = display_fd;
  display_poll->events = POLLIN;
  const int nfds = ipc_fd >= 0 ? 2 : 1;
  if (nfds == 2) {
    fds[1].fd = ipc_fd;
    fds[1].events = POLLIN;
  }

  while (state->running) {
    wl_display_flush(state->display);
    display_poll->events = POLLIN;
    if (nfds == 2) {
      fds[1].events = POLLIN;
    }

    /* Shorten the wait when a wheel action asked for an early refresh. */
    int timeout = 1000;
    if (g_refresh_at_ms != 0) {
      const long remaining = g_refresh_at_ms - mono_ms();
      timeout = remaining > 0 ? (int)remaining : 0;
      if (timeout > 1000) {
        timeout = 1000;
      }
    }
    const int poll_ret = poll(fds, (nfds_t)nfds, timeout);

    if (poll_ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "debar: poll failed\n");
      state->running = false;
      return -1;
    }

    if (poll_ret > 0) {
      if (display_poll->revents & POLLIN) {
        if (wl_display_dispatch(state->display) < 0 && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
          fprintf(stderr, "debar: Wayland event dispatch failed\n");
          state->running = false;
          return -1;
        }
      }
      if (nfds == 2 && (fds[1].revents & POLLIN)) {
        /* workspace events: update state and repaint immediately */
        ipc_handle_events(&wayland_data);
        wayland_redraw(state);
      }
      if ((display_poll->revents & POLLERR) != 0 ||
          (display_poll->revents & POLLHUP) != 0) {
        state->running = false;
        break;
      }
      if (nfds == 2 && (fds[1].revents & (POLLERR | POLLHUP)) != 0) {
        state->running = false;
        break;
      }
    }

    if (poll_ret == 0) {
      /* One-second tick (or an early refresh requested by a wheel action):
       * refresh live data and repaint. Workspaces are always re-queried so
       * occupancy ("has a program") stays accurate even when the event
       * stream is connected; the switch grace window keeps this poll from
       * reverting a just-clicked active workspace. */
      modules_update(&wayland_data);
      ipc_query_workspaces(&wayland_data);
      wayland_redraw(state);
      g_refresh_at_ms = 0;
      /* midnight rollover: move the "today" marker while the popup is open */
      if (calendar_refresh_today(&state->calendar) &&
          state->popup_surface != NULL) {
        popup_redraw(state);
      }
    }

    reap_retired_buffers(state);
  }

  return 0;
}

void wayland_finish(wayland_state_t *state) {
  if (state == NULL) {
    return;
  }

  ipc_finish();
  popup_close(state);
  if (state->keyboard != NULL) {
    wl_keyboard_destroy(state->keyboard);
    state->keyboard = NULL;
  }
  if (state->pointer != NULL) {
    wl_pointer_destroy(state->pointer);
    state->pointer = NULL;
  }
  if (state->seat != NULL) {
    wl_seat_destroy(state->seat);
    state->seat = NULL;
  }

  bar_buffer_t *buffer = state->buffers;
  while (buffer != NULL) {
    bar_buffer_t *next = buffer->next;
    render_buffer_destroy(buffer);
    buffer = next;
  }

  if (state->layer_surface != NULL) {
    zwlr_layer_surface_v1_destroy(state->layer_surface);
  }
  if (state->surface != NULL) {
    wl_surface_destroy(state->surface);
  }
  if (state->layer_shell != NULL) {
    zwlr_layer_shell_v1_destroy(state->layer_shell);
  }
  if (state->shm != NULL) {
    wl_shm_destroy(state->shm);
  }
  if (state->compositor != NULL) {
    wl_compositor_destroy(state->compositor);
  }
  if (state->registry != NULL) {
    wl_registry_destroy(state->registry);
  }
  if (state->display != NULL) {
    wl_display_disconnect(state->display);
  }

  memset(state, 0, sizeof(*state));
}
