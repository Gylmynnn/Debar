#ifndef DEBAR_RENDER_H
#define DEBAR_RENDER_H

#include "calendar.h"
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

/* Horizontal span of the clock pill inside the bar surface (same geometry as
 * render_bar). Used for click hit-testing and popup placement. */
bool render_clock_span(const bar_config_t *config, const modules_data_t *data,
                       double *x_out, double *w_out);

/* Draws the floating calendar popup (CAL_WIDTH x CAL_HEIGHT buffer). */
void render_calendar(bar_buffer_t *buffer, const bar_config_t *config,
                     const calendar_t *cal);

#endif
