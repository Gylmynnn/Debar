#ifndef DEBAR_RENDER_H
#define DEBAR_RENDER_H

#include "config.h"
#include "modules.h"
#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <wayland-client.h>

typedef struct bar_buffer {
  struct wl_buffer *wl_buffer;
  cairo_surface_t *surface;
  void *mapping;
  size_t size;
  bool retired;
  int width;
  int height;
  bool busy;
  struct bar_buffer *next;
} bar_buffer_t;

bar_buffer_t *render_buffer_create(struct wl_shm *shm, int width, int height);
void render_bar(bar_buffer_t *buffer, const bar_config_t *config,
                const modules_data_t *data);
void render_buffer_destroy(bar_buffer_t *buffer);

/* Hit-testing: which workspace ghost sits under (px, py)? Uses the same
 * geometry as the workspace pill in render_bar. Returns false when the
 * point is not over a workspace. */
bool render_workspace_hit(const bar_config_t *config, int bar_height,
                          const modules_data_t *data, double px, double py,
                          int *index_out);

#endif
