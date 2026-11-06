#define _GNU_SOURCE
#include "render.h"

#include <limits.h>
#include <linux/memfd.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static int create_shared_memory(size_t size)
{
    int fd = memfd_create("cbar-shm", MFD_CLOEXEC);
    if (fd < 0) {
        perror("memfd_create");
        return -1;
    }

    if (ftruncate(fd, (off_t)size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }

    return fd;
}

static void handle_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    bar_buffer_t *buffer = data;
    buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = handle_buffer_release,
};

bar_buffer_t *render_buffer_create(struct wl_shm *shm, int width, int height)
{
    if (shm == NULL || width <= 0 || height <= 0) {
        return NULL;
    }

    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    if (stride <= 0) {
        fprintf(stderr, "cbar: invalid buffer stride\n");
        return NULL;
    }
    const size_t size = (size_t)stride * (size_t)height;
    if (size > INT_MAX) {
        fprintf(stderr, "cbar: requested buffer is too large\n");
        return NULL;
    }
    const int fd = create_shared_memory(size);
    if (fd < 0) {
        return NULL;
    }

    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
    if (pool == NULL) {
        munmap(mapping, size);
        close(fd);
        return NULL;
    }

    struct wl_buffer *wl_buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (wl_buffer == NULL) {
        munmap(mapping, size);
        return NULL;
    }

    bar_buffer_t *buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) {
        wl_buffer_destroy(wl_buffer);
        munmap(mapping, size);
        return NULL;
    }

    buffer->wl_buffer = wl_buffer;
    buffer->mapping = mapping;
    buffer->size = size;
    buffer->width = width;
    buffer->height = height;
    buffer->surface = cairo_image_surface_create_for_data(
        mapping, CAIRO_FORMAT_ARGB32, width, height, stride);

    if (cairo_surface_status(buffer->surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(buffer->surface);
        wl_buffer_destroy(wl_buffer);
        munmap(mapping, size);
        free(buffer);
        return NULL;
    }

    wl_buffer_add_listener(wl_buffer, &buffer_listener, buffer);
    return buffer;
}

static void set_source_hex(cairo_t *cr, const char *hex)
{
    unsigned int red = 0, green = 0, blue = 0;
    if (hex == NULL || sscanf(hex, "#%2x%2x%2x", &red, &green, &blue) != 3) {
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        return;
    }

    cairo_set_source_rgb(cr, red / 255.0, green / 255.0, blue / 255.0);
}

void render_buffer_draw(bar_buffer_t *buffer, const bar_config_t *config,
                        const char *text)
{
    if (buffer == NULL || config == NULL || text == NULL) {
        return;
    }

    cairo_t *cr = cairo_create(buffer->surface);
    set_source_hex(cr, config->background);
    cairo_paint(cr);

    set_source_hex(cr, config->foreground);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string(config->font);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text, -1);

    int text_width = 0;
    int text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    (void)text_width;

    cairo_move_to(cr, config->padding,
                  (buffer->height - text_height) / 2.0);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_flush(buffer->surface);
}

void render_buffer_destroy(bar_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }

    if (buffer->wl_buffer != NULL) {
        wl_buffer_destroy(buffer->wl_buffer);
    }
    if (buffer->surface != NULL) {
        cairo_surface_destroy(buffer->surface);
    }
    if (buffer->mapping != NULL && buffer->mapping != MAP_FAILED) {
        munmap(buffer->mapping, buffer->size);
    }
    free(buffer);
}
