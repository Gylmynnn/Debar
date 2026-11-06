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

/* ---------------- pointer input ------------------------------------------ */

/* Debounce so one wheel tick does not skip several workspaces. */
#define SCROLL_MIN_INTERVAL_MS 180

static void pointer_handle_button(wayland_state_t *state) {
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

static void pointer_handle_axis(wayland_state_t *state, double value) {
  static struct timespec last = {0, 0};

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  const long now_ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;
  const long last_ms = last.tv_sec * 1000 + last.tv_nsec / 1000000;
  if (last_ms != 0 && now_ms - last_ms < SCROLL_MIN_INTERVAL_MS) {
    return;
  }

  const int count = wayland_data.ws_count > 0 ? (wayland_data.ws_count > DEBAR_MIN_WORKSPACES ? wayland_data.ws_count : DEBAR_MIN_WORKSPACES) : DEBAR_MIN_WORKSPACES;
  int target = wayland_data.ws_active + (value > 0.0 ? 1 : -1);
  if (target < 0) {
    target = 0;
  } else if (target >= count) {
    target = count - 1;
  }
  if (target == wayland_data.ws_active) {
    return;
  }

  if (ipc_switch_workspace(&wayland_data, target + 1)) {
    last = now;
    wayland_redraw(state);
  }
}


/* ---------------- Seat and pointer listeners ----------------------------- */

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t surface_x, wl_fixed_t surface_y) {
  (void)serial;
  (void)surface;
  (void)pointer;
  wayland_state_t *state = data;
  state->pointer_x = wl_fixed_to_double(surface_x);
  state->pointer_y = wl_fixed_to_double(surface_y);
  dlog("debar debug: pointer enter (%.1f, %.1f)", state->pointer_x,
       state->pointer_y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
  (void)data;
  (void)serial;
  (void)surface;
  (void)pointer;
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
  (void)button; /* any button switches the workspace under the pointer */
  if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
    pointer_handle_button((wayland_state_t *)data);
  }
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis,
                         wl_fixed_t value) {
  (void)pointer;
  (void)time;
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    pointer_handle_axis((wayland_state_t *)data, wl_fixed_to_double(value));
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

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t capabilities) {
  wayland_state_t *state = data;
  dlog("debar debug: seat capabilities=0x%x", capabilities);
  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0) {
    state->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(state->pointer, &pointer_listener, state);
    dlog("debar debug: pointer created");
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
    state->layer_shell =
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
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
  zwlr_layer_surface_v1_set_anchor(state->layer_surface,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  /* Floating bar: margins leave the wallpaper visible around it. */
  zwlr_layer_surface_v1_set_margin(state->layer_surface, config->margin_top,
                                   config->margin_side, 0, config->margin_side);
  zwlr_layer_surface_v1_set_exclusive_zone(state->layer_surface,
                                           config->exclusive);
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
  bar_buffer_t **link = &state->buffers;
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

    const int poll_ret = poll(fds, (nfds_t)nfds, 1000);

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
      /* One-second tick: refresh live data and repaint. Workspaces are
       * always re-queried so occupancy ("has a program") stays accurate
       * even when the event stream is connected; the switch grace window
       * keeps this poll from reverting a just-clicked active workspace. */
      modules_update(&wayland_data);
      ipc_query_workspaces(&wayland_data);
      wayland_redraw(state);
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
