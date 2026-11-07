#define _GNU_SOURCE
#include "render.h"

#include <limits.h>
#include <linux/memfd.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------- theme (sampled from the reference screenshot) ---------- */
static const char *COL_WHITE = "#ffffff";    /* numbers inside pills, ghosts */
static const char *COL_MINT = "#94e2d5";     /* cpu pill */
static const char *COL_SALMON = "#e57474";   /* ram pill, pac-man, firefox */
static const char *COL_GOLD = "#e5c76b";     /* crown, folder */
static const char *COL_CYAN = "#6cbfbf";     /* occupied workspace ghost */
static const char *COL_APP_GRN = "#8ccf7e";  /* shell hexagon */
static const char *COL_APP_BLUE = "#67b0e8"; /* vscode */
static const char *COL_VOL = "#a6e3a1";      /* speaker */
static const char *COL_BRI = "#89b4fa";      /* brightness */
static const char *COL_USB = "#458588";      /* usb tile */
static const char *COL_USB_DARK = "#3c3836";
static const char *COL_CREAM = "#ebdbb2"; /* wifi, bluetooth */
static const char *COL_BLACK = "#000000";

static int create_shared_memory(size_t size) {
  int fd = memfd_create("debar-shm", MFD_CLOEXEC);
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

static void handle_buffer_release(void *data, struct wl_buffer *wl_buffer) {
  (void)wl_buffer;
  bar_buffer_t *buffer = data;
  buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = handle_buffer_release,
};

bar_buffer_t *render_buffer_create(struct wl_shm *shm, int width, int height) {
  if (shm == NULL || width <= 0 || height <= 0) {
    return NULL;
  }

  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
  if (stride <= 0) {
    fprintf(stderr, "debar: invalid buffer stride\n");
    return NULL;
  }
  const size_t size = (size_t)stride * (size_t)height;
  if (size > INT_MAX) {
    fprintf(stderr, "debar: requested buffer is too large\n");
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
/* ---------------- low-level draw helpers -------------------------------- */

static bool parse_hex(const char *hex, double color[4]) {
  unsigned int red = 0, green = 0, blue = 0, alpha = 255;

  if (hex == NULL) {
    return false;
  }
  if (sscanf(hex, "#%2x%2x%2x%2x", &red, &green, &blue, &alpha) == 4) {
    /* full */
  } else if (sscanf(hex, "#%2x%2x%2x", &red, &green, &blue) == 3) {
    alpha = 255;
  } else {
    return false;
  }

  color[0] = red / 255.0;
  color[1] = green / 255.0;
  color[2] = blue / 255.0;
  color[3] = alpha / 255.0;
  return true;
}

static void set_source_hex_a(cairo_t *cr, const char *hex, double alpha) {
  double color[4];
  if (parse_hex(hex, color)) {
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3] * alpha);
    return;
  }
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
}

static void set_source_hex(cairo_t *cr, const char *hex) {
  set_source_hex_a(cr, hex, 1.0);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h,
                         double r) {
  if (r > w / 2.0) {
    r = w / 2.0;
  }
  if (r > h / 2.0) {
    r = h / 2.0;
  }

  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, M_PI * 1.5);
  cairo_close_path(cr);
}

/* Rectangle with independent left/right corner radii (0 = square corners).
 * Used for the two-tone cpu/ram pills. */
static void split_rect(cairo_t *cr, double x, double y, double w, double h,
                       double rl, double rr) {
  cairo_new_sub_path(cr);
  cairo_move_to(cr, x + rl, y);
  cairo_line_to(cr, x + w - rr, y);
  if (rr > 0.0) {
    cairo_arc(cr, x + w - rr, y + rr, rr, -M_PI / 2.0, 0.0);
  }
  cairo_line_to(cr, x + w, y + h - rr);
  if (rr > 0.0) {
    cairo_arc(cr, x + w - rr, y + h - rr, rr, 0.0, M_PI / 2.0);
  }
  cairo_line_to(cr, x + rl, y + h);
  if (rl > 0.0) {
    cairo_arc(cr, x + rl, y + h - rl, rl, M_PI / 2.0, M_PI);
  }
  cairo_line_to(cr, x, y + rl);
  if (rl > 0.0) {
    cairo_arc(cr, x + rl, y + rl, rl, M_PI, M_PI * 1.5);
  }
  cairo_close_path(cr);
}

static void draw_pill(cairo_t *cr, double x, double y, double w, double h,
                      double r, const char *bg) {
  rounded_rect(cr, x, y, w, h, r);
  set_source_hex(cr, bg);
  cairo_fill(cr);
}

static PangoFontDescription *make_font(const bar_config_t *config,
                                       PangoWeight weight) {
  PangoFontDescription *font = pango_font_description_new();
  pango_font_description_set_family(font, config->font_family);
  pango_font_description_set_weight(font, weight);
  pango_font_description_set_absolute_size(font,
                                           config->font_size * PANGO_SCALE);
  return font;
}

/* Measures and draws one label, vertically centered on mid_y.
 * Returns the rendered width. */
static double draw_label(cairo_t *cr, PangoFontDescription *font,
                         const char *text, double x, double mid_y,
                         const char *color) {
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text, -1);

  int text_width = 0;
  int text_height = 0;
  pango_layout_get_pixel_size(layout, &text_width, &text_height);

  set_source_hex(cr, color);
  cairo_move_to(cr, x, mid_y - text_height / 2.0);
  pango_cairo_show_layout(cr, layout);

  g_object_unref(layout);
  return (double)text_width;
}

static double measure_label(PangoFontDescription *font, const char *text) {
  /* Layouts need a cairo context; reuse a tiny throwaway surface purely
   * for measuring text widths. */
  static cairo_surface_t *surface = NULL;
  static cairo_t *cr = NULL;

  if (cr == NULL) {
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cr = cairo_create(surface);
  }

  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text, -1);

  int width = 0;
  int height = 0;
  pango_layout_get_pixel_size(layout, &width, &height);
  (void)height;
  g_object_unref(layout);
  return (double)width;
}

static void poly(cairo_t *cr, const double *pts, int n) {
  cairo_new_path(cr);
  cairo_move_to(cr, pts[0], pts[1]);
  for (int i = 1; i < n; i++) {
    cairo_line_to(cr, pts[i * 2], pts[i * 2 + 1]);
  }
  cairo_close_path(cr);
}

/* ---------------- icon glyphs (vector, no icon font required) ------------ */

/* Pac-man facing right with a pellet in front of the mouth. */
static void draw_pacman(cairo_t *cr, double cx, double cy, const char *color) {
  const double r = 8.2;

  set_source_hex(cr, color);
  cairo_new_path(cr);
  cairo_move_to(cr, cx + 0.5, cy);
  cairo_arc(cr, cx, cy, r, 0.62, 2.0 * M_PI - 0.62);
  cairo_close_path(cr);
  cairo_fill(cr);

  cairo_new_path(cr);
  cairo_arc(cr, cx + 5.6, cy + 0.2, 1.5, 0.0, 2.0 * M_PI);
  cairo_fill(cr);
}

/* 15x17 ghost. filled: solid body with punched-out eyes (occupied workspace);
 * otherwise a 1.6px outline with solid eyes (empty workspace). */
static void draw_ghost(cairo_t *cr, double cx, double cy, bool filled,
                       const char *body, const char *eyes) {
  const double lw = 1.6;
  const double inset = filled ? 0.0 : lw / 2.0;
  const double hw = 7.6 - inset;
  const double top = cy - 8.5 + inset;
  const double bottom = cy + 8.5 - inset;
  const double notch = 3.4;

  cairo_new_path(cr);
  cairo_move_to(cr, cx - hw, bottom);
  cairo_line_to(cr, cx - hw, top + hw);
  cairo_arc(cr, cx, top + hw, hw, M_PI, 2.0 * M_PI);
  cairo_line_to(cr, cx + hw, bottom);
  cairo_line_to(cr, cx + hw / 2.0, bottom - notch);
  cairo_line_to(cr, cx, bottom);
  cairo_line_to(cr, cx - hw / 2.0, bottom - notch);
  cairo_close_path(cr);

  set_source_hex(cr, body);
  if (filled) {
    cairo_fill(cr);
  } else {
    cairo_set_line_width(cr, lw);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
  }

  cairo_new_path(cr);
  cairo_arc(cr, cx - 2.3, cy - 1.3, 1.7, 0.0, 2.0 * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, cx + 2.3, cy - 1.3, 1.7, 0.0, 2.0 * M_PI);
  set_source_hex(cr, eyes);
  cairo_fill(cr);
}

static void draw_crown(cairo_t *cr, double cx, double cy) {
  /* 21 x 16 px, flat base, three tips with small round pearls */
  const double x0 = cx - 10.3;
  const double x1 = cx + 10.3;

  const double body[] = {
      x0 + 1.4, cy + 8.0, x0 + 0.2, cy - 4.2, cx - 5.4, cy + 0.9, cx,
      cy - 7.6, cx + 5.4, cy + 0.9, x1 - 0.2, cy - 4.2, x1 - 1.4, cy + 8.0,
  };
  poly(cr, body, 7);
  set_source_hex(cr, COL_GOLD);
  cairo_fill(cr);

  cairo_new_path(cr);
  cairo_arc(cr, x0 + 1.0, cy - 4.6, 1.4, 0, 2.0 * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, cx, cy - 7.2, 1.4, 0, 2.0 * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, x1 - 1.0, cy - 4.6, 1.4, 0, 2.0 * M_PI);
  cairo_fill(cr);
}

/* Hexagon vertex k (0 = top, clockwise). */
static void hex_vertex(double cx, double cy, double r, int k, double *x,
                       double *y) {
  const double a = -M_PI / 2.0 + k * M_PI / 3.0;
  *x = cx + r * cos(a);
  *y = cy + r * sin(a);
}

/* CPU glyph: dark isometric cube with mint edges, sits on the mint tile. */
static void draw_cube(cairo_t *cr, double cx, double cy, const char *line,
                      const char *fill) {
  const double r = 9.8;
  double v[6][2];
  for (int k = 0; k < 6; k++) {
    hex_vertex(cx, cy, r, k, &v[k][0], &v[k][1]);
  }

  cairo_new_path(cr);
  for (int k = 0; k < 6; k++) {
    if (k == 0) {
      cairo_move_to(cr, v[k][0], v[k][1]);
    } else {
      cairo_line_to(cr, v[k][0], v[k][1]);
    }
  }
  cairo_close_path(cr);
  set_source_hex(cr, fill);
  cairo_fill_preserve(cr);
  set_source_hex_a(cr, line, 0.75);
  cairo_set_line_width(cr, 1.0);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(cr);

  set_source_hex_a(cr, line, 0.9);
  for (int k = 0; k < 6; k += 2) {
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, v[k][0], v[k][1]);
    cairo_stroke(cr);
  }
}

/* RAM glyph: dark module with three cut-out chips and a pin strip. */
static void draw_ram(cairo_t *cr, double cx, double cy, const char *tile,
                     const char *fill) {
  const double w = 21.0;
  const double x = cx - w / 2.0;
  const double y = cy - 7.0;

  set_source_hex(cr, fill);
  rounded_rect(cr, x, y, w, 9.2, 1.2);
  cairo_fill(cr);
  rounded_rect(cr, x, y + 10.4, w, 3.6, 0.8);
  cairo_fill(cr);

  /* chips (punched out in the tile color) */
  set_source_hex(cr, tile);
  for (int i = 0; i < 3; i++) {
    cairo_rectangle(cr, x + 4.6 + 4.2 * i, y + 2.0, 2.7, 5.0);
  }
  /* pin notches */
  for (int i = 0; i < 5; i++) {
    cairo_rectangle(cr, x + 2.3 + 3.9 * i, y + 12.9, 1.2, 1.1);
  }
  cairo_fill(cr);
}

static void draw_speaker(cairo_t *cr, double cx, double cy, const char *color) {
  const double x0 = cx - 10.6;
  const double ax = x0 + 11.25; /* arcs are centered on the cone's edge */

  set_source_hex(cr, color);
  cairo_rectangle(cr, x0, cy - 3.1, 4.6, 6.2);
  cairo_fill(cr);

  const double cone[] = {
      x0 + 4.4, cy - 3.1, ax, cy - 7.5, ax, cy + 7.5, x0 + 4.4, cy + 3.1,
  };
  poly(cr, cone, 4);
  cairo_fill(cr);

  cairo_set_line_width(cr, 1.9);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_new_path(cr);
  cairo_arc(cr, ax, cy, 4.6, -0.95, 0.95);
  cairo_stroke(cr);
  cairo_new_path(cr);
  cairo_arc(cr, ax, cy, 9.0, -0.8, 0.8);
  cairo_stroke(cr);
}

/* Brightness glyph: 8-point sun with the right half hollowed out. */
static void draw_brightness(cairo_t *cr, double cx, double cy,
                            const char *color, const char *hole) {
  set_source_hex(cr, color);
  cairo_new_path(cr);
  for (int i = 0; i < 16; i++) {
    const double a = -M_PI / 2.0 + i * M_PI / 8.0;
    const double r = (i % 2 == 0) ? 8.6 : 6.6;
    const double px = cx + r * cos(a);
    const double py = cy + r * sin(a);
    if (i == 0) {
      cairo_move_to(cr, px, py);
    } else {
      cairo_line_to(cr, px, py);
    }
  }
  cairo_close_path(cr);
  cairo_fill(cr);

  set_source_hex(cr, hole);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, 4.6, -M_PI / 2.0, M_PI / 2.0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

/* --- app icon strip --- */

static void draw_shell_hex(cairo_t *cr, double cx, double cy) {
  const double r = 9.6;
  double v[6][2];
  for (int k = 0; k < 6; k++) {
    hex_vertex(cx, cy, r, k, &v[k][0], &v[k][1]);
  }

  /* outline */
  cairo_new_path(cr);
  for (int k = 0; k < 6; k++) {
    if (k == 0) {
      cairo_move_to(cr, v[k][0], v[k][1]);
    } else {
      cairo_line_to(cr, v[k][0], v[k][1]);
    }
  }
  cairo_close_path(cr);
  set_source_hex_a(cr, COL_APP_GRN, 0.6);
  cairo_set_line_width(cr, 1.0);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(cr);

  /* right-hand face */
  const double face[] = {
      cx, cy, v[1][0], v[1][1], v[2][0], v[2][1], v[3][0], v[3][1],
  };
  poly(cr, face, 4);
  set_source_hex(cr, COL_APP_GRN);
  cairo_fill(cr);

  /* tiny "$_" hint on the face */
  cairo_set_source_rgba(cr, 0.08, 0.11, 0.12, 0.85);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, cx + 2.6, cy + 1.4);
  cairo_line_to(cr, cx + 2.6, cy + 5.2);
  cairo_stroke(cr);
  cairo_move_to(cr, cx + 4.4, cy + 6.0);
  cairo_line_to(cr, cx + 6.6, cy + 6.0);
  cairo_stroke(cr);
}

static void draw_folder(cairo_t *cr, double cx, double cy) {
  const double back[] = {
      cx - 8.7, cy - 7.0, cx - 2.2, cy - 7.0, cx - 0.6, cy - 5.2,
      cx + 6.8, cy - 5.2, cx + 6.8, cy + 6.8, cx - 8.7, cy + 6.8,
  };
  poly(cr, back, 6);
  set_source_hex_a(cr, COL_GOLD, 0.7);
  cairo_fill(cr);

  const double front[] = {
      cx - 7.6, cy - 2.6, cx + 8.9, cy - 2.6,
      cx + 6.8, cy + 6.8, cx - 8.7, cy + 6.8,
  };
  poly(cr, front, 4);
  set_source_hex_a(cr, "#141b1e", 0.9);
  cairo_set_line_width(cr, 1.2);
  cairo_stroke_preserve(cr);
  set_source_hex(cr, COL_GOLD);
  cairo_fill(cr);
}

static void draw_vscode(cairo_t *cr, double cx, double cy, const char *hole) {
  set_source_hex(cr, COL_APP_BLUE);

  const double block[] = {
      cx + 3.3, cy - 8.4, cx + 8.6, cy - 5.9, cx + 8.6, cy + 5.9,
      cx + 3.3, cy + 8.4, cx - 4.4, cy + 1.6, cx - 4.4, cy - 1.6,
  };
  poly(cr, block, 6);
  cairo_fill(cr);

  cairo_set_line_width(cr, 2.3);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_move_to(cr, cx - 7.4, cy - 3.6);
  cairo_line_to(cr, cx - 3.0, cy + 0.2);
  cairo_stroke(cr);
  cairo_move_to(cr, cx - 7.4, cy + 3.6);
  cairo_line_to(cr, cx - 3.0, cy - 0.2);
  cairo_stroke(cr);

  const double notch[] = {
      cx + 3.3, cy - 3.6, cx + 3.3, cy + 3.6, cx - 0.9, cy,
  };
  poly(cr, notch, 3);
  set_source_hex(cr, hole);
  cairo_fill(cr);
}

static void draw_firefox(cairo_t *cr, double cx, double cy, const char *hole) {
  set_source_hex(cr, COL_SALMON);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, 8.8, 0.0, 2.0 * M_PI);
  cairo_fill(cr);

  /* off-center dark disc: thin rim top-right, thick body bottom-left */
  set_source_hex(cr, hole);
  cairo_new_path(cr);
  cairo_arc(cr, cx + 1.7, cy - 1.3, 5.5, 0.0, 2.0 * M_PI);
  cairo_fill(cr);

  /* swirl hint inside the globe */
  set_source_hex(cr, COL_SALMON);
  cairo_new_path(cr);
  cairo_arc(cr, cx + 2.6, cy + 0.2, 2.3, 0.0, 2.0 * M_PI);
  cairo_fill(cr);
  set_source_hex(cr, hole);
  cairo_new_path(cr);
  cairo_arc(cr, cx + 3.6, cy - 0.9, 1.3, 0.0, 2.0 * M_PI);
  cairo_fill(cr);
}

/* --- right-hand status icons --- */

static void draw_usb(cairo_t *cr, double cx, double cy) {
  rounded_rect(cr, cx - 11.5, cy - 11.0, 23.0, 22.0, 5.0);
  set_source_hex(cr, COL_USB);
  cairo_fill(cr);

  set_source_hex(cr, COL_CREAM);
  cairo_rectangle(cr, cx - 2.6, cy - 7.0, 5.2, 3.4);
  cairo_fill(cr);

  set_source_hex(cr, COL_USB_DARK);
  rounded_rect(cr, cx - 4.0, cy - 2.6, 8.0, 7.9, 1.4);
  cairo_fill(cr);
  cairo_rectangle(cr, cx - 1.4, cy + 5.0, 2.8, 5.6);
  cairo_fill(cr);
}

static void draw_wifi(cairo_t *cr, double cx, double cy) {
  const double apex = cy + 7.0;
  const double half = 0.6;

  cairo_new_path(cr);
  cairo_move_to(cr, cx, apex);
  cairo_arc(cr, cx, apex, 14.5, -M_PI / 2.0 - half, -M_PI / 2.0 + half);
  cairo_close_path(cr);
  set_source_hex_a(cr, COL_CREAM, 0.3);
  cairo_fill(cr);

  cairo_new_path(cr);
  cairo_move_to(cr, cx, apex);
  cairo_arc(cr, cx, apex, 7.0, -M_PI / 2.0 - half, -M_PI / 2.0 + half);
  cairo_close_path(cr);
  set_source_hex(cr, COL_CREAM);
  cairo_fill(cr);
}

/* Dark-mode toggle: white-rimmed black disc with a white bite on the right. */
static void draw_contrast(cairo_t *cr, double cx, double cy) {
  set_source_hex(cr, COL_WHITE);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, 10.0, 0.0, 2.0 * M_PI);
  cairo_fill(cr);

  set_source_hex(cr, COL_BLACK);
  cairo_new_path(cr);
  cairo_arc(cr, cx - 0.4, cy, 8.7, 0.0, 2.0 * M_PI);
  cairo_fill(cr);

  cairo_save(cr);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, 10.0, 0.0, 2.0 * M_PI);
  cairo_clip(cr);
  set_source_hex(cr, COL_WHITE);
  cairo_new_path(cr);
  cairo_arc(cr, cx + 4.4, cy, 6.4, 0.0, 2.0 * M_PI);
  cairo_fill(cr);
  cairo_restore(cr);
}

static void draw_bluetooth(cairo_t *cr, double cx, double cy) {
  rounded_rect(cr, cx - 5.2, cy - 8.0, 10.4, 16.0, 5.2);
  set_source_hex(cr, COL_CREAM);
  cairo_fill(cr);

  set_source_hex(cr, COL_USB_DARK);
  cairo_set_line_width(cr, 1.1);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
  cairo_new_path(cr);
  cairo_move_to(cr, cx - 3.0, cy - 3.0);
  cairo_line_to(cr, cx + 3.0, cy + 3.0);
  cairo_line_to(cr, cx, cy + 6.0);
  cairo_line_to(cr, cx, cy - 6.0);
  cairo_line_to(cr, cx + 3.0, cy - 3.0);
  cairo_line_to(cr, cx - 3.0, cy + 3.0);
  cairo_stroke(cr);
}

/* ---------------- layout ------------------------------------------------- */

#define WS_STEP 30.0        /* px between workspace icon centers */
#define WS_PILL_EXTRA 28.0 /* pill padding beyond count * WS_STEP */

bool render_workspace_hit(const bar_config_t *config, int bar_height,
                          const modules_data_t *data, double px, double py,
                          int *index_out) {
  (void)bar_height;
  (void)py;
  if (config == NULL || data == NULL || index_out == NULL) {
    return false;
  }

  const int count = data->ws_count > 0 ? (data->ws_count > DEBAR_MIN_WORKSPACES ? data->ws_count : DEBAR_MIN_WORKSPACES) : DEBAR_MIN_WORKSPACES;
  const double x0 = (double)config->padding.left;
  if (px < x0 + 2.0 || px > x0 + count * WS_STEP + WS_PILL_EXTRA) {
    return false;
  }

  /* ghost centers sit at x + WS_STEP * (i + 1); accept ± WS_STEP / 2 */
  int index = (int)((px - (x0 + WS_STEP / 2.0)) / WS_STEP);
  if (index < 0) {
    index = 0;
  } else if (index >= count) {
    index = count - 1;
  }
  *index_out = index;
  return true;
}

bool render_clock_span(const bar_config_t *config, const modules_data_t *data,
                       double *x_out, double *w_out) {
  if (config == NULL || data == NULL || x_out == NULL || w_out == NULL) {
    return false;
  }

  /* Same math as render_bar: workspace pill, gap, then the clock pill. */
  const int count = data->ws_count > 0 ? (data->ws_count > DEBAR_MIN_WORKSPACES ? data->ws_count : DEBAR_MIN_WORKSPACES) : DEBAR_MIN_WORKSPACES;
  const double ws_w = count * 30.0 + 28.0;

  PangoFontDescription *font = make_font(config, PANGO_WEIGHT_MEDIUM);
  const double text_w = measure_label(font, data->clock);
  pango_font_description_free(font);

  *x_out = (double)config->padding.left + ws_w + (double)config->gap;
  *w_out = 49.5 + text_w + 26.5;
  return true;
}

render_scroll_target_t render_scroll_hit(const bar_config_t *config,
                                         const modules_data_t *data,
                                         double bar_width, double px,
                                         double py) {
  if (config == NULL || data == NULL || bar_width <= 0.0) {
    return RENDER_SCROLL_NONE;
  }

  const double pill_y = config->padding.top;
  if (py < pill_y || py > pill_y + config->pill_height) {
    return RENDER_SCROLL_NONE;
  }

  const bool has_vol = data->volume[0] != '\0';
  const bool has_bri = data->brightness[0] != '\0';
  if (!has_vol && !has_bri) {
    return RENDER_SCROLL_NONE;
  }

  PangoFontDescription *font = make_font(config, PANGO_WEIGHT_BOLD);
  const double vol_text_w = has_vol ? measure_label(font, data->volume) : 0.0;
  const double bri_text_w =
      has_bri ? measure_label(font, data->brightness) : 0.0;
  pango_font_description_free(font);

  /* Same widths as the volume + brightness pill in render_bar. */
  const double vol_part = has_vol ? 21.25 + 12.0 + vol_text_w : 0.0;
  const double bri_part = has_bri ? 20.6 + 17.0 + 5.3 + bri_text_w : 0.0;
  const double w = 15.6 + vol_part + bri_part + 21.5;

  const double rw = bar_width - ((double)config->padding.right - 24.0);
  const double left = rw - 166.0 - w;
  if (px < left || px > left + w) {
    return RENDER_SCROLL_NONE;
  }

  if (has_vol && px <= left + 15.6 + vol_part) {
    return RENDER_SCROLL_VOLUME;
  }
  return has_bri ? RENDER_SCROLL_BRIGHTNESS : RENDER_SCROLL_NONE;
}

void render_bar(bar_buffer_t *buffer, const bar_config_t *config,
                const modules_data_t *data) {
  if (buffer == NULL || config == NULL || data == NULL) {
    return;
  }

  cairo_t *cr = cairo_create(buffer->surface);
  const double width = buffer->width;
  const double height = buffer->height;

  cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);

  /* start fully transparent: the bar floats, wallpaper shows around it */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  draw_pill(cr, 0.0, 0.0, width, height, config->radius, config->background);

  const double pill_h = config->pill_height;
  const double pill_y = config->padding.top;
  const double mid_y = pill_y + pill_h / 2.0;
  const double radius = pill_h / 2.0;
  const double gap = config->gap;
  const char *seg = config->segment;

  PangoFontDescription *font_clock = make_font(config, PANGO_WEIGHT_MEDIUM);
  PangoFontDescription *font_bold = make_font(config, PANGO_WEIGHT_BOLD);

  /* ---- left cluster -------------------------------------------------- */

  double x = config->padding.left;

  /* workspaces: pac-man = active, filled ghost = occupied, outline = empty */
  {
    const int count = data->ws_count > 0 ? (data->ws_count > DEBAR_MIN_WORKSPACES ? data->ws_count : DEBAR_MIN_WORKSPACES) : DEBAR_MIN_WORKSPACES;
    const double step = 30.0;
    const double w = count * step + 28.0;

    draw_pill(cr, x, pill_y, w, pill_h, radius, seg);
    for (int i = 0; i < count; i++) {
      const double cx = x + step * (i + 1);
      if (i == data->ws_active) {
        draw_pacman(cr, cx + 1.4, mid_y + 0.5, COL_SALMON);
      } else if (data->ws_occupied & (1u << i)) {
        draw_ghost(cr, cx, mid_y, true, COL_CYAN, seg);
      } else {
        draw_ghost(cr, cx, mid_y, false, COL_WHITE, COL_WHITE);
      }
    }
    x += w + gap;
  }

  /* clock pill */
  {
    const double text_w = measure_label(font_clock, data->clock);
    const double w = 49.5 + text_w + 26.5;

    draw_pill(cr, x, pill_y, w, pill_h, radius, seg);
    draw_crown(cr, x + 24.5, mid_y + 0.7);
    draw_label(cr, font_clock, data->clock, x + 49.5, mid_y,
               config->foreground);
    x += w + gap;
  }
  (void)x;

  /* ---- right cluster (laid out from the right edge) ------------------- */

  /* The offsets below were measured with a 24 px right padding; shift them
   * when the configured padding differs. */
  const double rw = width - ((double)config->padding.right - 24.0);

  /* status icons: usb, wifi, contrast, bluetooth (28 px pitch) */
  draw_usb(cr, rw - 134.5, mid_y - 0.6);
  draw_wifi(cr, rw - 106.5, mid_y - 0.6);
  draw_contrast(cr, rw - 78.5, mid_y - 0.5);
  draw_bluetooth(cr, rw - 50.5, mid_y - 0.6);

  /* volume + brightness pill */
  double vol_left;
  {
    const bool has_vol = data->volume[0] != '\0';
    const bool has_bri = data->brightness[0] != '\0';
    const double vol_w = has_vol ? measure_label(font_bold, data->volume) : 0.0;
    const double bri_w =
        has_bri ? measure_label(font_bold, data->brightness) : 0.0;

    double w = 15.6;
    if (has_vol) {
      w += 21.25 + 12.0 + vol_w;
    }
    if (has_bri) {
      w += 20.6 + 17.0 + 5.3 + bri_w;
    }
    w += 21.5;

    const double right = rw - 166.0;
    vol_left = right - w;
    draw_pill(cr, vol_left, pill_y, w, pill_h, radius, seg);

    double px = vol_left + 15.6;
    if (has_vol) {
      draw_speaker(cr, px + 10.6, mid_y - 0.2, COL_VOL);
      px += 21.25 + 12.0;
      draw_label(cr, font_bold, data->volume, px, mid_y, COL_WHITE);
      px += vol_w;
    }
    if (has_bri) {
      px += 20.6;
      draw_brightness(cr, px + 8.5, mid_y - 0.3, COL_BRI, seg);
      px += 17.0 + 5.3;
      draw_label(cr, font_bold, data->brightness, px, mid_y, COL_WHITE);
    }
  }

  /* decorative app icon strip (shell, files, editor, browser) */
  const double app_step = 38.3;
  const double app_y = mid_y + 0.4;
  const double app_last = vol_left - 31.6;
  const double app_first = app_last - 3.0 * app_step;
  draw_shell_hex(cr, app_first, app_y);
  draw_folder(cr, app_first + app_step, app_y);
  draw_vscode(cr, app_first + 2.0 * app_step, app_y - 0.4, config->background);
  draw_firefox(cr, app_last, app_y, config->background);

  /* ram pill (salmon tile + dark text half) */
  const double tile_ram = 53.0;
  const double ram_text_w = measure_label(font_bold, data->mem);
  const double ram_w = tile_ram + 15.0 + ram_text_w + 15.0;
  const double ram_x = (app_first - 31.5) - ram_w;
  {
    split_rect(cr, ram_x + tile_ram, pill_y, ram_w - tile_ram, pill_h, 0.0,
               radius);
    set_source_hex(cr, seg);
    cairo_fill(cr);
    split_rect(cr, ram_x, pill_y, tile_ram, pill_h, radius, 0.0);
    set_source_hex(cr, COL_SALMON);
    cairo_fill(cr);

    draw_ram(cr, ram_x + 30.5, mid_y + 0.3, COL_SALMON, seg);
    draw_label(cr, font_bold, data->mem, ram_x + tile_ram + 15.0, mid_y,
               COL_WHITE);
  }

  /* cpu pill (mint tile + dark text half) */
  {
    const double tile = 48.0;
    const double text_w = measure_label(font_bold, data->cpu);
    const double w = tile + 15.0 + text_w + 15.0;
    const double cx0 = ram_x - gap - w;

    split_rect(cr, cx0 + tile, pill_y, w - tile, pill_h, 0.0, radius);
    set_source_hex(cr, seg);
    cairo_fill(cr);
    split_rect(cr, cx0, pill_y, tile, pill_h, radius, 0.0);
    set_source_hex(cr, COL_MINT);
    cairo_fill(cr);

    draw_cube(cr, cx0 + 27.5, mid_y + 0.5, COL_MINT, seg);
    draw_label(cr, font_bold, data->cpu, cx0 + tile + 15.0, mid_y, COL_WHITE);
  }

  pango_font_description_free(font_clock);
  pango_font_description_free(font_bold);
  cairo_destroy(cr);
  cairo_surface_flush(buffer->surface);
}

/* ---------------- calendar popup ----------------------------------------- */

/* Everblush palette. Panel, text and separator follow the bar config
 * (#141b1e / #dadada / #232a2d); the rest are fixed Everblush tones. */
static const char *CAL_ACCENT = "#8ccf7e"; /* everblush green: header, today */
static const char *CAL_DIM = "#4f5456";    /* foreground #dadada at 30% */
static const char *CAL_HINT = "#636769";   /* foreground #dadada at 40% */

static PangoFontDescription *make_font_px(const bar_config_t *config,
                                          PangoWeight weight, double px) {
  PangoFontDescription *font = pango_font_description_new();
  pango_font_description_set_family(font, config->font_family);
  pango_font_description_set_weight(font, weight);
  pango_font_description_set_absolute_size(font, px * PANGO_SCALE);
  return font;
}

/* Draws text so that the *cap height* is centered on cap_cy (digits and
 * capitals look vertically centered, unlike centering the line box).
 * x is the left edge, or the center when centered is true.
 * Returns the rendered width. */
static double draw_text_cap(cairo_t *cr, PangoFontDescription *font, double px,
                            const char *text, double x, double cap_cy,
                            const char *color, bool centered) {
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text, -1);

  int w = 0;
  int h = 0;
  pango_layout_get_pixel_size(layout, &w, &h);
  const double baseline = pango_layout_get_baseline(layout) / (double)PANGO_SCALE;

  set_source_hex(cr, color);
  cairo_move_to(cr, centered ? x - w / 2.0 : x, cap_cy + px * 0.365 - baseline);
  pango_cairo_show_layout(cr, layout);

  g_object_unref(layout);
  return (double)w;
}

void render_calendar(bar_buffer_t *buffer, const bar_config_t *config,
                     const calendar_t *cal) {
  if (buffer == NULL || config == NULL || cal == NULL) {
    return;
  }

  cairo_t *cr = cairo_create(buffer->surface);
  const double width = buffer->width;
  const double height = buffer->height;
  const double mid_x = width / 2.0;

  cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  const char *CAL_BG = config->background;
  const char *CAL_TEXT = config->foreground;
  const char *CAL_RULE = config->segment;

  draw_pill(cr, 0.0, 0.0, width, height, 16.0, CAL_BG);

  PangoFontDescription *font_title = make_font_px(config, PANGO_WEIGHT_BOLD, 15.0);
  PangoFontDescription *font_head = make_font_px(config, PANGO_WEIGHT_BOLD, 13.0);
  PangoFontDescription *font_day = make_font_px(config, PANGO_WEIGHT_NORMAL, 13.0);
  PangoFontDescription *font_hint = make_font_px(config, PANGO_WEIGHT_NORMAL, 10.0);

  /* title: "November 2026" */
  char title[48];
  snprintf(title, sizeof(title), "%s %d", calendar_month_name(cal->month),
           cal->year);
  draw_text_cap(cr, font_title, 15.0, title, mid_x, 24.5, CAL_TEXT, true);

  /* grid geometry: 7 columns x 6 rows, centered in the panel */
  const double col0 = mid_x - 3.0 * 34.0;
  const double col_pitch = 34.0;
  const double head_y = 58.5;
  const double row0 = 84.0;
  const double row_pitch = 28.0;

  static const char *const WEEKDAYS[7] = {"Mo", "Tu", "We", "Th",
                                          "Fr", "Sa", "Su"};
  for (int c = 0; c < 7; c++) {
    draw_text_cap(cr, font_head, 13.0, WEEKDAYS[c], col0 + c * col_pitch,
                  head_y, CAL_ACCENT, true);
  }

  calendar_cell_t cells[42];
  calendar_build_grid(cal, cells);
  for (int i = 0; i < 42; i++) {
    const double cx = col0 + (i % 7) * col_pitch;
    const double cy = row0 + (i / 7) * row_pitch;
    char text[8];
    snprintf(text, sizeof(text), "%d", cells[i].day);

    if (cells[i].today) {
      /* 31 x 26 marker, same size as in the reference */
      rounded_rect(cr, cx - 15.0, cy - 13.0, 31.0, 26.0, 6.0);
      if (cal->style == 0) {
        set_source_hex(cr, CAL_ACCENT);
        cairo_fill(cr);
        draw_text_cap(cr, font_head, 13.0, text, cx, cy + 0.5, CAL_BG, true);
      } else {
        set_source_hex(cr, CAL_ACCENT);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
        draw_text_cap(cr, font_head, 13.0, text, cx, cy + 0.5, CAL_ACCENT, true);
      }
    } else {
      draw_text_cap(cr, font_day, 13.0, text, cx, cy + 0.5,
                    cells[i].month_offset == 0 ? CAL_TEXT : CAL_DIM, true);
    }
  }

  /* footer: separator + key hints, centered as one group */
  set_source_hex(cr, CAL_RULE);
  cairo_rectangle(cr, 50.0, 249.0, 199.0, 1.0);
  cairo_fill(cr);

  static const char *const HINTS[4] = {"\xe2\x86\x90\xe2\x86\x92 mo",
                                       "\xe2\x86\x91\xe2\x86\x93 yr",
                                       "\xe2\x86\xb5 today", "s style"};
  const double hint_gap = 19.0;
  double widths[4];
  double total = hint_gap * 3.0;
  for (int i = 0; i < 4; i++) {
    widths[i] = measure_label(font_hint, HINTS[i]);
    total += widths[i];
  }
  double hx = mid_x - total / 2.0;
  for (int i = 0; i < 4; i++) {
    draw_text_cap(cr, font_hint, 10.0, HINTS[i], hx, 265.5, CAL_HINT, false);
    hx += widths[i] + hint_gap;
  }

  pango_font_description_free(font_title);
  pango_font_description_free(font_head);
  pango_font_description_free(font_day);
  pango_font_description_free(font_hint);
  cairo_destroy(cr);
  cairo_surface_flush(buffer->surface);
}

void render_buffer_destroy(bar_buffer_t *buffer) {
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
