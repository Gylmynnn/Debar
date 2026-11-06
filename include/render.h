#ifndef CBAR_RENDER_H
#define CBAR_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <cairo.h>
#include <wayland-client.h>
#include "config.h"

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
void render_buffer_draw(bar_buffer_t *buffer, const bar_config_t *config,
                        const char *text);
void render_buffer_destroy(bar_buffer_t *buffer);

#endif
