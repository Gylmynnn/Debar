#include "wayland.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static modules_data_t wayland_data; /* live data, updated every second */

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
    fprintf(stderr, "cbar: cannot connect to Wayland display\n");
    return false;
  }

  state->registry = wl_display_get_registry(state->display);
  if (state->registry == NULL) {
    fprintf(stderr, "cbar: cannot get Wayland registry\n");
    return false;
  }

  wl_registry_add_listener(state->registry, &registry_listener, state);
  if (wl_display_roundtrip(state->display) < 0) {
    fprintf(stderr, "cbar: registry roundtrip failed\n");
    return false;
  }

  if (state->compositor == NULL || state->shm == NULL ||
      state->layer_shell == NULL) {
    fprintf(stderr,
            "cbar: compositor, wl_shm, or wlr-layer-shell is unavailable\n");
    return false;
  }

  state->surface = wl_compositor_create_surface(state->compositor);
  if (state->surface == NULL) {
    fprintf(stderr, "cbar: cannot create wl_surface\n");
    return false;
  }

  state->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      state->layer_shell, state->surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
      "cbar");
  if (state->layer_surface == NULL) {
    fprintf(stderr, "cbar: cannot create layer surface\n");
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
    fprintf(stderr, "cbar: failed to create Wayland buffer\n");
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

  const int fd = wl_display_get_fd(state->display);
  if (fd < 0) {
    fprintf(stderr, "cbar: cannot get display fd\n");
    return -1;
  }

  while (state->running) {
    wl_display_flush(state->display);

    struct pollfd fds = {.fd = fd, .events = POLLIN, .revents = 0};
    const int poll_ret = poll(&fds, 1, 1000);

    if (poll_ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "cbar: poll failed\n");
      state->running = false;
      return -1;
    }

    if (poll_ret == 0) {
      /* one-second tick: refresh live data and repaint */
      modules_update(&wayland_data);
      wayland_redraw(state);
    } else if (fds.revents & POLLIN) {
      if (wl_display_dispatch(state->display) < 0 && errno != EAGAIN &&
          errno != EWOULDBLOCK) {
        fprintf(stderr, "cbar: Wayland event dispatch failed\n");
        state->running = false;
        return -1;
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
