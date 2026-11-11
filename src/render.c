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
static const char *COL_GOLD = "#e5c76b";     /* crown */
static const char *COL_CYAN = "#6cbfbf";     /* occupied workspace ghost */
static const char *COL_VOL = "#a6e3a1";      /* speaker */
static const char *COL_BRI = "#89b4fa";      /* brightness */
static const char *COL_BT = "#67b0e8";     /* wifi glyph (everblush blue) */
// static const char *COL_WIFI = "#c47fd5";       /* bluetooth glyph (everblush magenta) */
static const char *COL_BORDER = "#232a2d";   /* popup border (everblush gray) */
static const char *COL_MUTED = "#4f5456";    /* speaker while muted */

/* Nerd Font glyphs (Font Awesome range, present in every patched font). */
#define NF_WIFI "\xef\x87\xab"      /* U+F1EB  nf-fa-wifi */
// #define NF_BLUETOOTH "\xef\x8a\x94" /* U+F294  nf-fa-bluetooth_b */
#define NF_BLUETOOTH "\uf293"   /* atau "\xEF\x8A\x94" untuk UTF-8 */
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

/* --- Wi-Fi / Bluetooth glyphs (Nerd Font) --- */

/* Draws one Nerd Font glyph centered on (cx, cy) using its ink box, so icons
 * of different widths line up. The configured font is tried first, then the
 * standalone Symbols Nerd Font as fallback. */
static void draw_nerd_icon(cairo_t *cr, const bar_config_t *config, double cx,
                           double cy, const char *glyph, double size,
                           const char *color, double alpha) {
  char family[256];
  snprintf(family, sizeof(family), "%s,Symbols Nerd Font,Symbols Nerd Font Mono",
           config->font_family);

  PangoFontDescription *font = pango_font_description_new();
  pango_font_description_set_family(font, family);
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);

  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, glyph, -1);

  PangoRectangle ink;
  pango_layout_get_extents(layout, &ink, NULL);
  const double ink_cx = (ink.x + ink.width / 2.0) / (double)PANGO_SCALE;
  const double ink_cy = (ink.y + ink.height / 2.0) / (double)PANGO_SCALE;

  set_source_hex_a(cr, color, alpha);
  cairo_move_to(cr, cx - ink_cx, cy - ink_cy);
  pango_cairo_show_layout(cr, layout);

  g_object_unref(layout);
  pango_font_description_free(font);
}

/* Border drawn just inside the popup edge, following its rounded corners. */
static void draw_popup_border(cairo_t *cr, double width, double height,
                              double radius) {
  const double lw = 2.0;
  rounded_rect(cr, lw / 2.0, lw / 2.0, width - lw, height - lw,
               radius - lw / 2.0);
  set_source_hex(cr, COL_BORDER);
  cairo_set_line_width(cr, lw);
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

/* ---- right-cluster geometry (shared by drawing and hit-testing) --------- */

/* One system tray: the icons of the running StatusNotifierItems sit directly
 * on the bar (no pill) at a 28 px pitch, right-aligned. The volume pill and
 * everything left of it shift as icons come and go. With four icons this is
 * exactly where the old hard-coded status icons were. */
#define TRAY_SLOT_WIDTH 28.0
#define TRAY_RIGHT_INSET 36.5 /* right edge of the last slot, from the edge */
#define TRAY_NEXT_GAP 17.5    /* first slot -> next pill */
#define TRAY_ICON_SIZE 22.0

/* The offsets were measured with a 24 px right padding; shift them when the
 * configured padding differs. */
static double right_edge(const bar_config_t *config, double bar_width) {
  return bar_width - ((double)config->padding.right - 24.0);
}

static double tray_width(int count) {
  return count > 0 ? (double)count * TRAY_SLOT_WIDTH : 0.0;
}

static double tray_left(const bar_config_t *config, int count,
                        double bar_width) {
  return right_edge(config, bar_width) - TRAY_RIGHT_INSET - tray_width(count);
}

static double volume_pill_right(const bar_config_t *config, int tray_items,
                                double bar_width) {
  const double left = tray_left(config, tray_items, bar_width);
  return tray_items > 0 ? left - TRAY_NEXT_GAP : left;
}

static void draw_tray_icon(cairo_t *cr, cairo_surface_t *icon, double cx,
                           double cy, double size) {
  if (icon == NULL) {
    /* unresolved icon: a small dot keeps the slot visible/clickable */
    set_source_hex(cr, COL_WHITE);
    cairo_arc(cr, cx, cy, 3.0, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    return;
  }

  const int w = cairo_image_surface_get_width(icon);
  const int h = cairo_image_surface_get_height(icon);
  if (w <= 0 || h <= 0) {
    return;
  }

  const double scale = size / (double)(w > h ? w : h);
  cairo_save(cr);
  cairo_translate(cr, cx - (double)w * scale / 2.0,
                  cy - (double)h * scale / 2.0);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, icon, 0.0, 0.0);
  cairo_paint(cr);
  cairo_restore(cr);
}

/* Width of the volume + brightness pill; render_bar uses the same terms. */
static double volume_pill_width(const bar_config_t *config,
                                const modules_data_t *data) {
  const bool has_vol = data->volume[0] != '\0';
  const bool has_bri = data->brightness[0] != '\0';

  PangoFontDescription *font = make_font(config, PANGO_WEIGHT_BOLD);
  const double vol_text_w = has_vol ? measure_label(font, data->volume) : 0.0;
  const double bri_text_w =
      has_bri ? measure_label(font, data->brightness) : 0.0;
  pango_font_description_free(font);

  const double vol_part = has_vol ? 21.25 + 12.0 + vol_text_w : 0.0;
  const double bri_part = has_bri ? 20.6 + 17.0 + 5.3 + bri_text_w : 0.0;
  return 15.6 + vol_part + bri_part + 21.5;
}

/* The icon strip: Wi-Fi and Bluetooth, ending 31.6 px left of the volume
 * pill. */
#define APP_STEP 38.3
#define APP_COUNT 2
#define APP_WIFI_INDEX 0
#define APP_BLUETOOTH_INDEX 1

static double app_strip_last(const bar_config_t *config,
                             const modules_data_t *data, int tray_items,
                             double bar_width) {
  const double right = volume_pill_right(config, tray_items, bar_width);
  return right - volume_pill_width(config, data) - 31.6;
}

render_status_icon_t render_status_icon_hit(const bar_config_t *config,
                                            const modules_data_t *data,
                                            tray_t *tray, double bar_width,
                                            double px, double py,
                                            double *center_x) {
  if (config == NULL || data == NULL || bar_width <= 0.0) {
    return RENDER_ICON_NONE;
  }
  const double pill_y = config->padding.top;
  if (py < pill_y || py > pill_y + config->pill_height) {
    return RENDER_ICON_NONE;
  }

  const double last = app_strip_last(config, data, tray_count(tray), bar_width);
  const double first = last - (APP_COUNT - 1) * APP_STEP;
  const double wifi_cx = first + APP_WIFI_INDEX * APP_STEP;
  const double bt_cx = first + APP_BLUETOOTH_INDEX * APP_STEP;

  if (px >= wifi_cx - APP_STEP / 2.0 && px < wifi_cx + APP_STEP / 2.0) {
    if (center_x != NULL) {
      *center_x = wifi_cx;
    }
    return RENDER_ICON_WIFI;
  }
  if (px >= bt_cx - APP_STEP / 2.0 && px < bt_cx + APP_STEP / 2.0) {
    if (center_x != NULL) {
      *center_x = bt_cx;
    }
    return RENDER_ICON_BLUETOOTH;
  }
  return RENDER_ICON_NONE;
}

/* The CPU and RAM pills, laid out right to left from the Wi-Fi/Bluetooth
 * strip exactly like render_bar does. */
render_sys_pill_t render_sys_pill_hit(const bar_config_t *config,
                                      const modules_data_t *data, tray_t *tray,
                                      double bar_width, double px, double py,
                                      double *center_x) {
  if (config == NULL || data == NULL || bar_width <= 0.0) {
    return RENDER_SYS_NONE;
  }
  const double pill_y = config->padding.top;
  if (py < pill_y || py > pill_y + config->pill_height) {
    return RENDER_SYS_NONE;
  }

  const double last = app_strip_last(config, data, tray_count(tray), bar_width);
  const double app_first = last - (APP_COUNT - 1) * APP_STEP;

  PangoFontDescription *font = make_font(config, PANGO_WEIGHT_BOLD);
  const double ram_w = 53.0 + 15.0 + measure_label(font, data->mem) + 15.0;
  const double cpu_w = 48.0 + 15.0 + measure_label(font, data->cpu) + 15.0;
  pango_font_description_free(font);

  const double ram_x = (app_first - 31.5) - ram_w;
  const double cpu_x = ram_x - config->gap - cpu_w;

  if (px >= ram_x && px <= ram_x + ram_w) {
    if (center_x != NULL) {
      *center_x = ram_x + ram_w / 2.0;
    }
    return RENDER_SYS_MEM;
  }
  if (px >= cpu_x && px <= cpu_x + cpu_w) {
    if (center_x != NULL) {
      *center_x = cpu_x + cpu_w / 2.0;
    }
    return RENDER_SYS_CPU;
  }
  return RENDER_SYS_NONE;
}

render_scroll_target_t render_scroll_hit(const bar_config_t *config,
                                         const modules_data_t *data,
                                         tray_t *tray, double bar_width,
                                         double px, double py) {
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

  const double w = volume_pill_width(config, data);
  PangoFontDescription *font = make_font(config, PANGO_WEIGHT_BOLD);
  const double vol_part =
      has_vol ? 21.25 + 12.0 + measure_label(font, data->volume) : 0.0;
  pango_font_description_free(font);

  const double right = volume_pill_right(config, tray_count(tray), bar_width);
  const double left = right - w;
  if (px < left || px > left + w) {
    return RENDER_SCROLL_NONE;
  }

  if (has_vol && px <= left + 15.6 + vol_part) {
    return RENDER_SCROLL_VOLUME;
  }
  return has_bri ? RENDER_SCROLL_BRIGHTNESS : RENDER_SCROLL_NONE;
}

render_scroll_target_t render_level_pill_hit(const bar_config_t *config,
                                             const modules_data_t *data,
                                             tray_t *tray, double bar_width,
                                             double px, double py,
                                             double *center_x) {
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

  /* Same terms as render_bar: speaker + label, then (after a 20.6 px gap)
   * the sun + label. */
  PangoFontDescription *font = make_font(config, PANGO_WEIGHT_BOLD);
  const double vol_part =
      has_vol ? 21.25 + 12.0 + measure_label(font, data->volume) : 0.0;
  const double bri_part =
      has_bri ? 17.0 + 5.3 + measure_label(font, data->brightness) : 0.0;
  pango_font_description_free(font);

  const double w = volume_pill_width(config, data);
  const double right = volume_pill_right(config, tray_count(tray), bar_width);
  const double left = right - w;
  if (px < left || px > left + w) {
    return RENDER_SCROLL_NONE;
  }

  const double vol_end = left + 15.6 + vol_part;
  if (has_vol && (!has_bri || px <= vol_end + 10.3)) {
    if (center_x != NULL) {
      *center_x = left + 15.6 + vol_part / 2.0;
    }
    return RENDER_SCROLL_VOLUME;
  }
  if (center_x != NULL) {
    *center_x = vol_end + 20.6 + bri_part / 2.0;
  }
  return RENDER_SCROLL_BRIGHTNESS;
}

bool render_tray_hit(const bar_config_t *config, tray_t *tray,
                     double bar_width, double px, double py, int *index_out) {
  if (config == NULL || tray == NULL || index_out == NULL ||
      bar_width <= 0.0) {
    return false;
  }

  const int count = tray_count(tray);
  if (count <= 0) {
    return false;
  }

  const double pill_y = config->padding.top;
  if (py < pill_y || py > pill_y + config->pill_height) {
    return false;
  }

  const double left = tray_left(config, count, bar_width);
  const double rel = px - left;
  if (rel < 0.0 || rel >= tray_width(count)) {
    return false;
  }

  int index = (int)(rel / TRAY_SLOT_WIDTH);
  if (index < 0) {
    index = 0;
  } else if (index >= count) {
    index = count - 1;
  }
  *index_out = index;
  return true;
}

void render_bar(bar_buffer_t *buffer, const bar_config_t *config,
                const modules_data_t *data, tray_t *tray) {
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

  /* Dynamic tray snapshot: holds references until the frame is finished. */
  cairo_surface_t *tray_icons[DEBAR_TRAY_MAX_ITEMS];
  const int tray_items =
      tray_snapshot(tray, tray_icons, DEBAR_TRAY_MAX_ITEMS);

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

  /* system tray: one icon per running StatusNotifierItem, drawn straight on
   * the bar */
  if (tray_items > 0) {
    const double left = tray_left(config, tray_items, width);
    double icon_size = TRAY_ICON_SIZE;
    if (icon_size > pill_h - 8.0) {
      icon_size = pill_h - 8.0;
    }
    for (int i = 0; i < tray_items; i++) {
      const double cx = left + TRAY_SLOT_WIDTH * ((double)i + 0.5);
      draw_tray_icon(cr, tray_icons[i], cx, mid_y, icon_size);
    }
  }

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

    const double right = volume_pill_right(config, tray_items, width);
    vol_left = right - w;
    draw_pill(cr, vol_left, pill_y, w, pill_h, radius, seg);

    double px = vol_left + 15.6;
    if (has_vol) {
      draw_speaker(cr, px + 10.6, mid_y - 0.2,
                   data->volume_muted ? COL_MUTED : COL_VOL);
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

  /* Wi-Fi and Bluetooth (Nerd Font glyphs) open their manager popups; an
   * icon is dimmed while its radio is known to be off. */
  const double app_step = APP_STEP;
  const double app_last = vol_left - 31.6;
  const double app_first = app_last - (APP_COUNT - 1) * app_step;
  draw_nerd_icon(cr, config, app_first + APP_WIFI_INDEX * app_step, mid_y,
                 NF_WIFI, 18.0, COL_SALMON, data->net_wifi == 1 ? 0.35 : 1.0);
  draw_nerd_icon(cr, config, app_first + APP_BLUETOOTH_INDEX * app_step, mid_y,
                 NF_BLUETOOTH, 20.0, COL_BT, data->net_bt == 1 ? 0.35 : 1.0);

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

  for (int i = 0; i < tray_items; i++) {
    if (tray_icons[i] != NULL) {
      cairo_surface_destroy(tray_icons[i]);
    }
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
  draw_popup_border(cr, width, height, 16.0);

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


/* ---------------- Wi-Fi / Bluetooth manager popups ------------------------- */

/* Layout, in popup pixels. Shared by drawing and render_netpopup_hit(). */
#define NP_PAD 22.0
#define NP_HEADER_H 60.0
#define NP_LIST_TOP 68.0
#define NP_ROW_H 44.0
#define NP_FOOTER_Y 340.0
#define NP_TOGGLE_X 254.0
#define NP_TOGGLE_Y 18.0
#define NP_TOGGLE_W 44.0
#define NP_TOGGLE_H 24.0
#define NP_FIELD_X 22.0
#define NP_FIELD_Y 134.0
#define NP_FIELD_W 276.0
#define NP_FIELD_H 40.0

netpopup_hit_kind_t render_netpopup_hit(double px, double py,
                                        bool password_mode, int *row_out) {
  if (px < 0.0 || px >= NETPOPUP_WIDTH || py < 0.0 || py >= NETPOPUP_HEIGHT) {
    return NETPOPUP_HIT_NONE;
  }

  if (px >= NP_TOGGLE_X - 6.0 && px <= NP_TOGGLE_X + NP_TOGGLE_W + 6.0 &&
      py >= NP_TOGGLE_Y - 6.0 && py <= NP_TOGGLE_Y + NP_TOGGLE_H + 6.0) {
    return NETPOPUP_HIT_TOGGLE;
  }

  if (password_mode) {
    if (px >= NP_FIELD_X && px <= NP_FIELD_X + NP_FIELD_W &&
        py >= NP_FIELD_Y && py <= NP_FIELD_Y + NP_FIELD_H) {
      return NETPOPUP_HIT_FIELD;
    }
    return NETPOPUP_HIT_NONE;
  }

  const double list_bottom = NP_LIST_TOP + NETPOPUP_ROWS * NP_ROW_H;
  if (px >= 10.0 && px <= NETPOPUP_WIDTH - 10.0 && py >= NP_LIST_TOP &&
      py < list_bottom) {
    if (row_out != NULL) {
      *row_out = (int)((py - NP_LIST_TOP) / NP_ROW_H);
    }
    return NETPOPUP_HIT_ROW;
  }
  return NETPOPUP_HIT_NONE;
}

/* Text whose cap height is centered on cap_cy, cut with "..." at max_w. */
static void draw_text_fit(cairo_t *cr, PangoFontDescription *font, double px,
                          const char *text, double x, double cap_cy,
                          double max_w, const char *color) {
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text, -1);
  pango_layout_set_width(layout, (int)(max_w * PANGO_SCALE));
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

  const double baseline = pango_layout_get_baseline(layout) / (double)PANGO_SCALE;
  set_source_hex(cr, color);
  cairo_move_to(cr, x, cap_cy + px * 0.365 - baseline);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);
}

static void draw_signal_bars(cairo_t *cr, double cx, double cy, int lit,
                             const char *on, const char *off) {
  static const double heights[4] = {4.0, 7.0, 10.0, 13.0};
  const double x0 = cx - 9.0;
  const double base = cy + 6.5;
  for (int i = 0; i < 4; i++) {
    rounded_rect(cr, x0 + i * 5.0, base - heights[i], 3.0, heights[i], 1.0);
    set_source_hex(cr, i < lit ? on : off);
    cairo_fill(cr);
  }
}

static void draw_lock(cairo_t *cr, double cx, double cy, const char *color) {
  set_source_hex(cr, color);
  rounded_rect(cr, cx - 4.5, cy - 1.0, 9.0, 7.0, 1.5);
  cairo_fill(cr);
  cairo_set_line_width(cr, 1.4);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy - 1.0, 3.0, M_PI, 2.0 * M_PI);
  cairo_stroke(cr);
}

/* On/off switch with its top-left corner at (sx, sy). */
static void draw_switch_at(cairo_t *cr, double sx, double sy, bool on,
                           bool hover, const bar_config_t *config) {
  rounded_rect(cr, sx, sy, NP_TOGGLE_W, NP_TOGGLE_H, NP_TOGGLE_H / 2.0);
  if (on) {
    set_source_hex(cr, CAL_ACCENT);
  } else {
    set_source_hex(cr, hover ? "#3b4346" : "#2d3437");
  }
  cairo_fill(cr);

  const double r = NP_TOGGLE_H / 2.0 - 3.0;
  const double cx =
      on ? sx + NP_TOGGLE_W - NP_TOGGLE_H / 2.0 : sx + NP_TOGGLE_H / 2.0;
  set_source_hex(cr, on ? config->background : CAL_HINT);
  cairo_new_path(cr);
  cairo_arc(cr, cx, sy + NP_TOGGLE_H / 2.0, r, 0.0, 2.0 * M_PI);
  cairo_fill(cr);
}

static void draw_switch(cairo_t *cr, bool on, bool hover,
                        const bar_config_t *config) {
  draw_switch_at(cr, NP_TOGGLE_X, NP_TOGGLE_Y, on, hover, config);
}

typedef struct net_row {
  char label[64];
  bool active;
  bool lock;
  int bars; /* 0..4 for Wi-Fi, -1 for a Bluetooth device */
} net_row_t;

typedef struct net_popup_view {
  const char *title;
  const char *off_text;  /* "Wi-Fi is off" */
  const char *empty_text;
  const char *idle_hint;
  bool available;
  bool enabled;
  bool loaded;
  bool busy;
  const char *unavailable_text;
  const char *status;
  bool status_error;
  const net_row_t *rows;
  int row_count;
} net_popup_view_t;

static void render_net_popup(bar_buffer_t *buffer, const bar_config_t *config,
                             const net_popup_view_t *view,
                             const netpopup_ui_t *ui) {
  cairo_t *cr = cairo_create(buffer->surface);
  const double width = buffer->width;
  const double height = buffer->height;

  cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  draw_pill(cr, 0.0, 0.0, width, height, 16.0, config->background);
  draw_popup_border(cr, width, height, 16.0);

  PangoFontDescription *font_title = make_font_px(config, PANGO_WEIGHT_BOLD, 15.0);
  PangoFontDescription *font_bold = make_font_px(config, PANGO_WEIGHT_BOLD, 13.0);
  PangoFontDescription *font_row = make_font_px(config, PANGO_WEIGHT_NORMAL, 13.0);
  PangoFontDescription *font_small = make_font_px(config, PANGO_WEIGHT_NORMAL, 11.0);
  PangoFontDescription *font_tag = make_font_px(config, PANGO_WEIGHT_BOLD, 10.0);

  /* header: title + on/off switch */
  draw_text_cap(cr, font_title, 15.0, view->title, NP_PAD, 30.0,
                config->foreground, false);
  if (view->available) {
    draw_switch(cr, view->enabled, ui->hover_kind == NETPOPUP_HIT_TOGGLE,
                config);
  }
  set_source_hex(cr, config->segment);
  cairo_rectangle(cr, 18.0, NP_HEADER_H, width - 36.0, 1.0);
  cairo_fill(cr);

  const double mid_x = width / 2.0;

  if (ui->password_mode) {
    draw_text_cap(cr, font_small, 11.0, "Password for", NP_PAD, 92.0, CAL_HINT,
                  false);
    draw_text_fit(cr, font_bold, 13.0, ui->password_ssid, NP_PAD, 112.0,
                  width - 2.0 * NP_PAD, config->foreground);

    rounded_rect(cr, NP_FIELD_X, NP_FIELD_Y, NP_FIELD_W, NP_FIELD_H, 8.0);
    set_source_hex(cr, config->segment);
    cairo_fill_preserve(cr);
    set_source_hex(cr, CAL_ACCENT);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    const double cy = NP_FIELD_Y + NP_FIELD_H / 2.0;
    double caret_x = NP_FIELD_X + 14.0;
    if (ui->password_len == 0) {
      draw_text_cap(cr, font_row, 13.0, "Password", caret_x + 6.0, cy, CAL_DIM,
                    false);
    } else {
      const int shown = ui->password_len > 20 ? 20 : ui->password_len;
      set_source_hex(cr, config->foreground);
      for (int i = 0; i < shown; i++) {
        cairo_new_path(cr);
        cairo_arc(cr, caret_x + 3.0 + i * 12.0, cy, 3.2, 0.0, 2.0 * M_PI);
        cairo_fill(cr);
      }
      caret_x += shown * 12.0;
    }
    set_source_hex(cr, CAL_ACCENT);
    cairo_rectangle(cr, caret_x, cy - 8.0, 1.5, 16.0);
    cairo_fill(cr);

    draw_text_cap(cr, font_small, 11.0, "Enter  connect      Esc  cancel", NP_PAD,
                  198.0, CAL_HINT, false);
  } else {
    const char *message = NULL;
    if (!view->available) {
      message = view->unavailable_text;
    } else if (!view->enabled) {
      message = view->off_text;
    } else if (!view->loaded) {
      message = "Looking...";
    } else if (view->row_count == 0) {
      message = view->empty_text;
    }

    if (message != NULL) {
      draw_text_cap(cr, font_row, 13.0, message, mid_x, 200.0, CAL_HINT, true);
    } else {
      for (int slot = 0; slot < NETPOPUP_ROWS; slot++) {
        const int index = ui->scroll + slot;
        if (index >= view->row_count) {
          break;
        }
        const net_row_t *row = &view->rows[index];
        const double ry = NP_LIST_TOP + slot * NP_ROW_H;
        const double cy = ry + NP_ROW_H / 2.0;
        const bool hover = ui->hover_kind == NETPOPUP_HIT_ROW &&
                           ui->hover_row == slot;

        if (hover || row->active) {
          rounded_rect(cr, 10.0, ry + 2.0, width - 20.0, NP_ROW_H - 4.0, 8.0);
          set_source_hex(cr, config->segment);
          cairo_fill(cr);
        }

        const char *icon_on = row->active ? CAL_ACCENT : config->foreground;
        if (row->bars >= 0) {
          draw_signal_bars(cr, 34.0, cy, row->bars, icon_on, CAL_DIM);
        } else {
          draw_nerd_icon(cr, config, 34.0, cy, NF_BLUETOOTH, 18.0, COL_BT, 1.0);
        }

        double right = width - NP_PAD;
        if (row->active) {
          const double tag_w = measure_label(font_tag, "Connected");
          draw_text_cap(cr, font_tag, 10.0, "Connected", right - tag_w, cy,
                        CAL_ACCENT, false);
          right -= tag_w + 10.0;
        } else if (row->lock) {
          draw_lock(cr, width - NP_PAD - 4.0, cy, CAL_HINT);
          right -= 22.0;
        }
        draw_text_fit(cr, row->active ? font_bold : font_row, 13.0, row->label,
                      56.0, cy, right - 56.0, config->foreground);
      }

      if (view->row_count > NETPOPUP_ROWS) {
        const double track = NETPOPUP_ROWS * NP_ROW_H - 8.0;
        const double thumb = track * NETPOPUP_ROWS / (double)view->row_count;
        const double span = view->row_count - NETPOPUP_ROWS;
        const double y = NP_LIST_TOP + 4.0 +
                         (track - thumb) * (span > 0 ? ui->scroll / span : 0.0);
        rounded_rect(cr, width - 6.0, y, 3.0, thumb, 1.5);
        set_source_hex(cr, CAL_DIM);
        cairo_fill(cr);
      }
    }
  }

  /* footer: separator + status line */
  set_source_hex(cr, config->segment);
  cairo_rectangle(cr, 18.0, NP_FOOTER_Y, width - 36.0, 1.0);
  cairo_fill(cr);

  const char *status = view->status != NULL ? view->status : "";
  const char *shown = status[0] != '\0' ? status
                      : view->busy      ? "Working..."
                                        : view->idle_hint;
  draw_text_fit(cr, font_small, 11.0, shown, NP_PAD, 364.0, width - 2.0 * NP_PAD,
                view->status_error ? COL_SALMON : CAL_HINT);

  pango_font_description_free(font_title);
  pango_font_description_free(font_bold);
  pango_font_description_free(font_row);
  pango_font_description_free(font_small);
  pango_font_description_free(font_tag);
  cairo_destroy(cr);
  cairo_surface_flush(buffer->surface);
}

static int signal_bars(int signal) {
  return signal >= 75 ? 4 : signal >= 50 ? 3 : signal >= 25 ? 2 : signal > 0 ? 1 : 0;
}

void render_wifi_popup(bar_buffer_t *buffer, const bar_config_t *config,
                       const netmgr_wifi_t *wifi, const netpopup_ui_t *ui) {
  if (buffer == NULL || config == NULL || wifi == NULL || ui == NULL) {
    return;
  }

  net_row_t rows[NETMGR_MAX_ITEMS];
  for (int i = 0; i < wifi->count; i++) {
    snprintf(rows[i].label, sizeof(rows[i].label), "%s", wifi->nets[i].ssid);
    rows[i].active = wifi->nets[i].active;
    rows[i].lock = wifi->nets[i].secure;
    rows[i].bars = signal_bars(wifi->nets[i].signal);
  }

  const net_popup_view_t view = {
      .title = "Wi-Fi",
      .off_text = "Wi-Fi is off",
      .empty_text = "No networks found",
      .idle_hint = "Click a network to connect",
      .available = wifi->available || !wifi->loaded,
      .enabled = wifi->enabled || !wifi->loaded,
      .loaded = wifi->loaded,
      .busy = wifi->busy,
      .unavailable_text = wifi->status[0] != '\0' ? wifi->status : "Wi-Fi unavailable",
      .status = wifi->available ? wifi->status : "",
      .status_error = wifi->status_error,
      .rows = rows,
      .row_count = wifi->count,
  };
  render_net_popup(buffer, config, &view, ui);
}

void render_bt_popup(bar_buffer_t *buffer, const bar_config_t *config,
                     const netmgr_bt_t *bt, const netpopup_ui_t *ui) {
  if (buffer == NULL || config == NULL || bt == NULL || ui == NULL) {
    return;
  }

  net_row_t rows[NETMGR_MAX_ITEMS];
  for (int i = 0; i < bt->count; i++) {
    snprintf(rows[i].label, sizeof(rows[i].label), "%s", bt->devs[i].name);
    rows[i].active = bt->devs[i].connected;
    rows[i].lock = false;
    rows[i].bars = -1;
  }

  const net_popup_view_t view = {
      .title = "Bluetooth",
      .off_text = "Bluetooth is off",
      .empty_text = "No paired devices",
      .idle_hint = "Click a device to connect",
      .available = bt->available || !bt->loaded,
      .enabled = bt->powered || !bt->loaded,
      .loaded = bt->loaded,
      .busy = bt->busy,
      .unavailable_text = bt->status[0] != '\0' ? bt->status : "Bluetooth unavailable",
      .status = bt->available ? bt->status : "",
      .status_error = bt->status_error,
      .rows = rows,
      .row_count = bt->count,
  };
  render_net_popup(buffer, config, &view, ui);
}

/* ---------------- CPU / RAM popups ----------------------------------------- */

/* Same look as the calendar and the Wi-Fi/Bluetooth popups: bar background,
 * gray border, 16 px corners, a header with a rule under it and a footer. The
 * CPU popup is mint and the RAM popup salmon, like their pills. */
#define SP_PAD 22.0
#define SP_HEADER_H 60.0
#define SP_COL1_X 22.0  /* three stat columns */
#define SP_COL2_X 118.0
#define SP_COL3_X 214.0
#define SP_ROW_PITCH 30.0 /* program rows: name + value, meter below */
#define SP_CORE_PITCH 22.0
#define SP_MEM_HEIGHT 408

typedef struct sys_fonts {
  PangoFontDescription *title;
  PangoFontDescription *bold;
  PangoFontDescription *row;
  PangoFontDescription *small;
  PangoFontDescription *small_bold;
} sys_fonts_t;

static sys_fonts_t sys_fonts_new(const bar_config_t *config) {
  return (sys_fonts_t){
      .title = make_font_px(config, PANGO_WEIGHT_BOLD, 15.0),
      .bold = make_font_px(config, PANGO_WEIGHT_BOLD, 13.0),
      .row = make_font_px(config, PANGO_WEIGHT_NORMAL, 13.0),
      .small = make_font_px(config, PANGO_WEIGHT_NORMAL, 11.0),
      .small_bold = make_font_px(config, PANGO_WEIGHT_BOLD, 11.0),
  };
}

static void sys_fonts_free(sys_fonts_t *f) {
  pango_font_description_free(f->title);
  pango_font_description_free(f->bold);
  pango_font_description_free(f->row);
  pango_font_description_free(f->small);
  pango_font_description_free(f->small_bold);
}

static cairo_t *sys_popup_begin(bar_buffer_t *buffer,
                                const bar_config_t *config) {
  cairo_t *cr = cairo_create(buffer->surface);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  draw_pill(cr, 0.0, 0.0, buffer->width, buffer->height, 16.0,
            config->background);
  draw_popup_border(cr, buffer->width, buffer->height, 16.0);
  return cr;
}

static void sys_rule(cairo_t *cr, const bar_config_t *config, double width,
                     double y) {
  set_source_hex(cr, config->segment);
  cairo_rectangle(cr, 18.0, y, width - 36.0, 1.0);
  cairo_fill(cr);
}

static void draw_text_right(cairo_t *cr, PangoFontDescription *font, double px,
                            const char *text, double right, double cap_cy,
                            const char *color) {
  const double w = measure_label(font, text);
  draw_text_cap(cr, font, px, text, right - w, cap_cy, color, false);
}

/* Horizontal meter: a track with a rounded fill. A tiny non-zero value still
 * shows as a dot, so "almost idle" is not mistaken for "no data". */
static void draw_meter(cairo_t *cr, double x, double y, double w, double h,
                       double frac, const char *fill, const char *track) {
  rounded_rect(cr, x, y, w, h, h / 2.0);
  set_source_hex(cr, track);
  cairo_fill(cr);

  if (frac > 1.0) {
    frac = 1.0;
  }
  if (frac <= 0.004) {
    return;
  }
  double fw = w * frac;
  if (fw < h) {
    fw = h;
  }
  rounded_rect(cr, x, y, fw, h, h / 2.0);
  set_source_hex(cr, fill);
  cairo_fill(cr);
}

/* Per-core load colors: calm, busy, saturated. */
static const char *heat_color(double pct) {
  if (pct >= 85.0) {
    return COL_SALMON;
  }
  return pct >= 60.0 ? COL_GOLD : COL_MINT;
}

static void format_kib(char *out, size_t size, double kib) {
  if (kib >= 1024.0 * 1024.0) {
    snprintf(out, size, "%.1f GiB", kib / (1024.0 * 1024.0));
  } else {
    snprintf(out, size, "%.0f MiB", kib / 1024.0);
  }
}

static void format_uptime(char *out, size_t size, long seconds) {
  const long days = seconds / 86400;
  const long hours = (seconds % 86400) / 3600;
  const long minutes = (seconds % 3600) / 60;
  if (days > 0) {
    snprintf(out, size, "%ldd %ldh", days, hours);
  } else if (hours > 0) {
    snprintf(out, size, "%ldh %ldm", hours, minutes);
  } else {
    snprintf(out, size, "%ldm", minutes);
  }
}

/* A small label above a value; dot (optional) is a legend swatch. */
static void draw_stat(cairo_t *cr, const bar_config_t *config,
                      const sys_fonts_t *f, double x, double label_y,
                      double value_y, const char *label, const char *value,
                      const char *dot, double dot_alpha) {
  double label_x = x;
  if (dot != NULL) {
    set_source_hex_a(cr, dot, dot_alpha);
    cairo_new_path(cr);
    cairo_arc(cr, x + 3.5, label_y, 3.5, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    label_x += 12.0;
  }
  draw_text_cap(cr, f->small, 11.0, label, label_x, label_y, CAL_HINT, false);
  draw_text_cap(cr, f->bold, 13.0, value, x, value_y, config->foreground,
                false);
}

/* Ranked program list: rank, name, value, and a meter under each row. */
static void draw_proc_rows(cairo_t *cr, const bar_config_t *config,
                           const sys_fonts_t *f, double width, double top,
                           const sysinfo_proc_t *procs, int count, bool is_mem,
                           double scale, const char *accent,
                           const char *empty_text) {
  if (count <= 0) {
    draw_text_cap(cr, f->row, 13.0, empty_text, width / 2.0, top + 28.0,
                  CAL_HINT, true);
    return;
  }
  if (scale <= 0.0) {
    scale = 1.0;
  }

  const double name_x = 40.0;
  const double right = width - SP_PAD;
  for (int i = 0; i < count; i++) {
    const double ry = top + i * SP_ROW_PITCH;
    const double cy = ry + 8.0;

    char rank[16];
    snprintf(rank, sizeof(rank), "%d", i + 1);
    draw_text_cap(cr, f->small, 11.0, rank, SP_PAD, cy, CAL_DIM, false);

    char value[24];
    if (is_mem) {
      format_kib(value, sizeof(value), procs[i].value);
    } else {
      snprintf(value, sizeof(value), "%.1f%%", procs[i].value);
    }
    const double value_w = measure_label(f->bold, value);
    draw_text_right(cr, f->bold, 13.0, value, right, cy, config->foreground);
    draw_text_fit(cr, f->row, 13.0, procs[i].name, name_x, cy,
                  right - value_w - 12.0 - name_x, config->foreground);

    draw_meter(cr, name_x, ry + 19.0, right - name_x, 4.0,
               procs[i].value / scale, accent, config->segment);
  }
}

/* ---- RAM popup ---- */

int render_mem_popup_height(void) { return SP_MEM_HEIGHT; }

void render_mem_popup(bar_buffer_t *buffer, const bar_config_t *config,
                      const sysinfo_mem_t *mem) {
  if (buffer == NULL || config == NULL || mem == NULL) {
    return;
  }

  cairo_t *cr = sys_popup_begin(buffer, config);
  const double width = buffer->width;
  const double bar_w = width - 2.0 * SP_PAD;
  sys_fonts_t f = sys_fonts_new(config);
  const char *fg = config->foreground;

  draw_text_cap(cr, f.title, 15.0, "Memory", SP_PAD, 30.0, fg, false);
  sys_rule(cr, config, width, SP_HEADER_H);

  if (!mem->valid || mem->total_kb <= 0) {
    draw_text_cap(cr, f.row, 13.0, "Memory info unavailable", width / 2.0,
                  200.0, CAL_HINT, true);
    goto done;
  }

  {
    const double total = (double)mem->total_kb;
    char text[48];
    char text2[96];

    snprintf(text, sizeof(text), "%.0f%%", 100.0 * mem->used_kb / total);
    draw_text_right(cr, f.title, 15.0, text, width - SP_PAD, 30.0, COL_SALMON);

    /* usage bar: used (solid), reclaimable cache (faded), free (track) */
    const double used_f = (double)mem->used_kb / total;
    const double cache_f = (double)mem->cache_kb / total;
    cairo_save(cr);
    rounded_rect(cr, SP_PAD, 76.0, bar_w, 10.0, 5.0);
    set_source_hex(cr, config->segment);
    cairo_fill_preserve(cr);
    cairo_clip(cr);
    set_source_hex(cr, COL_SALMON);
    cairo_rectangle(cr, SP_PAD, 76.0, bar_w * used_f, 10.0);
    cairo_fill(cr);
    set_source_hex_a(cr, COL_SALMON, 0.38);
    cairo_rectangle(cr, SP_PAD + bar_w * used_f, 76.0, bar_w * cache_f, 10.0);
    cairo_fill(cr);
    cairo_restore(cr);

    format_kib(text, sizeof(text), (double)mem->used_kb);
    draw_stat(cr, config, &f, SP_COL1_X, 106.0, 124.0, "Used", text,
              COL_SALMON, 1.0);
    format_kib(text, sizeof(text), (double)mem->cache_kb);
    draw_stat(cr, config, &f, SP_COL2_X, 106.0, 124.0, "Cache", text,
              COL_SALMON, 0.38);
    format_kib(text, sizeof(text), (double)mem->free_kb);
    draw_stat(cr, config, &f, SP_COL3_X, 106.0, 124.0, "Free", text, CAL_DIM,
              1.0);

    /* swap */
    draw_text_cap(cr, f.small, 11.0, "Swap", SP_PAD, 152.0, CAL_HINT, false);
    if (mem->swap_total_kb > 0) {
      format_kib(text, sizeof(text), (double)mem->swap_used_kb);
      format_kib(text2, sizeof(text2), (double)mem->swap_total_kb);
      char both[160];
      snprintf(both, sizeof(both), "%s / %s", text, text2);
      draw_text_right(cr, f.small, 11.0, both, width - SP_PAD, 152.0, fg);
      draw_meter(cr, SP_PAD, 164.0, bar_w, 5.0,
                 (double)mem->swap_used_kb / (double)mem->swap_total_kb,
                 COL_SALMON, config->segment);
    } else {
      draw_text_right(cr, f.small, 11.0, "none", width - SP_PAD, 152.0,
                      CAL_HINT);
    }

    sys_rule(cr, config, width, 184.0);

    /* the programs that hold the most memory */
    draw_text_cap(cr, f.small, 11.0, "Top programs", SP_PAD, 202.0, CAL_HINT,
                  false);
    const double biggest = mem->top_count > 0 ? mem->top[0].value : 1.0;
    draw_proc_rows(cr, config, &f, width, 214.0, mem->top, mem->top_count,
                   true, biggest, COL_SALMON, "No data");

    /* footer */
    sys_rule(cr, config, width, 372.0);
    format_kib(text, sizeof(text), total);
    snprintf(text2, sizeof(text2), "Total %s", text);
    draw_text_cap(cr, f.small, 11.0, text2, SP_PAD, 392.0, CAL_HINT, false);
    format_kib(text, sizeof(text), total - (double)mem->used_kb);
    snprintf(text2, sizeof(text2), "Available %s", text);
    draw_text_right(cr, f.small, 11.0, text2, width - SP_PAD, 392.0, CAL_HINT);
  }

done:
  sys_fonts_free(&f);
  cairo_destroy(cr);
  cairo_surface_flush(buffer->surface);
}

/* ---- CPU popup ---- */

typedef struct cpu_layout {
  int rows; /* core rows: cores are listed in two columns */
  double sep1;
  double cores_label;
  double grid_top;
  double sep2;
  double top_label;
  double top_rows;
  double footer_sep;
  double footer_text;
  double height;
} cpu_layout_t;

/* Shared by drawing and render_cpu_popup_height(), so the popup is always
 * exactly as tall as its content. */
static cpu_layout_t cpu_layout(int cores) {
  if (cores < 1) {
    cores = 1;
  } else if (cores > SYSINFO_MAX_CORES) {
    cores = SYSINFO_MAX_CORES;
  }

  cpu_layout_t l;
  l.rows = (cores + 1) / 2;
  l.sep1 = 116.0;
  l.cores_label = 134.0;
  l.grid_top = 148.0;
  l.sep2 = l.grid_top + l.rows * SP_CORE_PITCH + 6.0;
  l.top_label = l.sep2 + 18.0;
  l.top_rows = l.sep2 + 30.0;
  l.footer_sep = l.top_rows + SYSINFO_CPU_TOP * SP_ROW_PITCH + 4.0;
  l.footer_text = l.footer_sep + 20.0;
  l.height = l.footer_sep + 36.0;
  return l;
}

int render_cpu_popup_height(int cores) {
  return (int)(cpu_layout(cores).height + 0.5);
}

void render_cpu_popup(bar_buffer_t *buffer, const bar_config_t *config,
                      const sysinfo_cpu_t *cpu) {
  if (buffer == NULL || config == NULL || cpu == NULL) {
    return;
  }

  cairo_t *cr = sys_popup_begin(buffer, config);
  const double width = buffer->width;
  sys_fonts_t f = sys_fonts_new(config);
  const char *fg = config->foreground;
  const int cores = cpu->cores > 0 ? cpu->cores : 1;
  const cpu_layout_t l = cpu_layout(cores);
  char text[48];

  /* header: title + total load */
  draw_text_cap(cr, f.title, 15.0, "CPU", SP_PAD, 30.0, fg, false);
  if (cpu->valid) {
    snprintf(text, sizeof(text), "%.0f%%", cpu->total);
  } else {
    snprintf(text, sizeof(text), "--");
  }
  draw_text_right(cr, f.title, 15.0, text, width - SP_PAD, 30.0, COL_MINT);
  sys_rule(cr, config, width, SP_HEADER_H);

  /* load average, temperature, frequency */
  snprintf(text, sizeof(text), "%.2f", cpu->load[0]);
  draw_stat(cr, config, &f, SP_COL1_X, 80.0, 98.0, "Load", text, NULL, 1.0);
  if (cpu->temp_c >= 0.0) {
    snprintf(text, sizeof(text), "%.0f\xc2\xb0""C", cpu->temp_c);
  } else {
    snprintf(text, sizeof(text), "--");
  }
  draw_stat(cr, config, &f, SP_COL2_X, 80.0, 98.0, "Temp", text, NULL, 1.0);
  if (cpu->freq_mhz >= 1000.0) {
    snprintf(text, sizeof(text), "%.1f GHz", cpu->freq_mhz / 1000.0);
  } else if (cpu->freq_mhz > 0.0) {
    snprintf(text, sizeof(text), "%.0f MHz", cpu->freq_mhz);
  } else {
    snprintf(text, sizeof(text), "--");
  }
  draw_stat(cr, config, &f, SP_COL3_X, 80.0, 98.0, "Freq", text, NULL, 1.0);
  sys_rule(cr, config, width, l.sep1);

  /* one meter per core, listed top to bottom in two columns */
  draw_text_cap(cr, f.small, 11.0, "Cores", SP_PAD, l.cores_label, CAL_HINT,
                false);
  for (int i = 0; i < cores && i < SYSINFO_MAX_CORES; i++) {
    const double x0 = i / l.rows == 0 ? 22.0 : 168.0;
    const double cy = l.grid_top + (i % l.rows) * SP_CORE_PITCH + 8.0;
    const double pct = cpu->valid ? cpu->core[i] : 0.0;

    snprintf(text, sizeof(text), "%d", i);
    draw_text_cap(cr, f.small, 11.0, text, x0, cy, CAL_HINT, false);
    draw_meter(cr, x0 + 24.0, cy - 3.0, 66.0, 6.0, pct / 100.0,
               heat_color(pct), config->segment);
    if (cpu->valid) {
      snprintf(text, sizeof(text), "%.0f%%", pct);
    } else {
      snprintf(text, sizeof(text), "--");
    }
    draw_text_right(cr, f.small_bold, 11.0, text, x0 + 130.0, cy, fg);
  }
  sys_rule(cr, config, width, l.sep2);

  /* the programs that use the most CPU */
  draw_text_cap(cr, f.small, 11.0, "Top processes", SP_PAD, l.top_label,
                CAL_HINT, false);
  double scale = cpu->top_count > 0 ? cpu->top[0].value : 5.0;
  if (scale < 5.0) {
    scale = 5.0; /* near-idle programs get short meters, not full ones */
  }
  draw_proc_rows(cr, config, &f, width, l.top_rows, cpu->top, cpu->top_count,
                 false, scale, COL_MINT, cpu->valid ? "Idle" : "Sampling...");

  /* footer */
  sys_rule(cr, config, width, l.footer_sep);
  char up[32];
  format_uptime(up, sizeof(up), cpu->uptime_s);
  snprintf(text, sizeof(text), "Uptime %s", up);
  draw_text_cap(cr, f.small, 11.0, text, SP_PAD, l.footer_text, CAL_HINT,
                false);
  snprintf(text, sizeof(text), "%d processes", cpu->procs);
  draw_text_right(cr, f.small, 11.0, text, width - SP_PAD, l.footer_text,
                  CAL_HINT);

  sys_fonts_free(&f);
  cairo_destroy(cr);
  cairo_surface_flush(buffer->surface);
}

/* ---------------- volume / brightness popups -------------------------------- */

/* Same look as the CPU/RAM popups (bar background, gray border, 16 px
 * corners, header with a rule under it, footer with key hints). The accent is
 * the pill's own color: green for volume, blue for brightness. Layout, in
 * popup pixels, is shared by drawing and render_levelpopup_hit(). */
#define LP_SLIDER_X0 64.0 /* the speaker / sun icon sits left of the track */
#define LP_SLIDER_CY 100.0
#define LP_SLIDER_HIT_H 34.0
#define LP_PRESETS 5
#define LP_PRESET_Y 136.0
#define LP_PRESET_H 32.0
#define LP_PRESET_GAP 8.0
#define LP_RULE_Y 188.0 /* rule above the mute row / the footer */
#define LP_MUTE_ROW_Y 196.0
#define LP_MUTE_ROW_H 40.0
#define LP_FOOTER_PAD 36.0 /* footer rule -> bottom edge, as in the CPU popup */

static const int LP_VOLUME_PRESETS[LP_PRESETS] = {0, 25, 50, 75, 100};
/* never 0: a black screen is hard to recover from */
static const int LP_BRIGHTNESS_PRESETS[LP_PRESETS] = {10, 25, 50, 75, 100};

static double lp_footer_sep(bool is_volume) {
  return is_volume ? LP_RULE_Y + 56.0 : LP_RULE_Y; /* + the mute row */
}

int render_level_popup_height(bool is_volume) {
  return (int)(lp_footer_sep(is_volume) + LP_FOOTER_PAD + 0.5);
}

static double lp_slider_x1(void) { return LEVELPOPUP_WIDTH - SP_PAD; }

int render_levelpopup_slider_value(double px) {
  const double x0 = LP_SLIDER_X0;
  const double x1 = lp_slider_x1();
  double frac = (px - x0) / (x1 - x0);
  if (frac < 0.0) {
    frac = 0.0;
  } else if (frac > 1.0) {
    frac = 1.0;
  }
  return (int)(frac * 100.0 + 0.5);
}

static double lp_preset_width(void) {
  return (LEVELPOPUP_WIDTH - 2.0 * SP_PAD - (LP_PRESETS - 1) * LP_PRESET_GAP) /
         LP_PRESETS;
}

levelpopup_hit_kind_t render_levelpopup_hit(bool is_volume, double px,
                                            double py, int *value_out,
                                            int *index_out) {
  if (px < 0.0 || px >= LEVELPOPUP_WIDTH ||
      py < 0.0 || py >= render_level_popup_height(is_volume)) {
    return LEVELPOPUP_HIT_NONE;
  }

  /* the slider's hit box is taller and wider than its track, so the knob is
   * easy to grab */
  if (py >= LP_SLIDER_CY - LP_SLIDER_HIT_H / 2.0 &&
      py <= LP_SLIDER_CY + LP_SLIDER_HIT_H / 2.0 &&
      px >= LP_SLIDER_X0 - 10.0 && px <= lp_slider_x1() + 10.0) {
    if (value_out != NULL) {
      *value_out = render_levelpopup_slider_value(px);
    }
    return LEVELPOPUP_HIT_SLIDER;
  }

  if (py >= LP_PRESET_Y && py <= LP_PRESET_Y + LP_PRESET_H) {
    const double bw = lp_preset_width();
    for (int i = 0; i < LP_PRESETS; i++) {
      const double bx = SP_PAD + i * (bw + LP_PRESET_GAP);
      if (px >= bx && px <= bx + bw) {
        if (value_out != NULL) {
          *value_out = (is_volume ? LP_VOLUME_PRESETS : LP_BRIGHTNESS_PRESETS)[i];
        }
        if (index_out != NULL) {
          *index_out = i;
        }
        return LEVELPOPUP_HIT_PRESET;
      }
    }
    return LEVELPOPUP_HIT_NONE;
  }

  if (is_volume && px >= 10.0 && px <= LEVELPOPUP_WIDTH - 10.0 &&
      py >= LP_MUTE_ROW_Y && py <= LP_MUTE_ROW_Y + LP_MUTE_ROW_H) {
    return LEVELPOPUP_HIT_MUTE;
  }
  return LEVELPOPUP_HIT_NONE;
}

static void render_level_popup(bar_buffer_t *buffer, const bar_config_t *config,
                               bool is_volume, int percent, bool muted,
                               const levelpopup_ui_t *ui) {
  cairo_t *cr = sys_popup_begin(buffer, config);
  const double width = buffer->width;
  sys_fonts_t f = sys_fonts_new(config);
  const char *fg = config->foreground;
  const char *accent = is_volume ? COL_VOL : COL_BRI;
  const bool available = percent >= 0;
  const bool dim = is_volume && muted; /* muted: gray instead of green */
  const char *fill = dim ? COL_MUTED : accent;
  char text[48];

  /* header: title + value */
  draw_text_cap(cr, f.title, 15.0, is_volume ? "Volume" : "Brightness",
                SP_PAD, 30.0, fg, false);
  if (available) {
    if (dim) {
      snprintf(text, sizeof(text), "Muted");
    } else {
      snprintf(text, sizeof(text), "%d%%", percent);
    }
    draw_text_right(cr, f.title, 15.0, text, width - SP_PAD, 30.0,
                    dim ? CAL_HINT : accent);
  }
  sys_rule(cr, config, width, SP_HEADER_H);

  if (!available) {
    draw_text_cap(cr, f.row, 13.0,
                  is_volume ? "Volume unavailable" : "No backlight found",
                  width / 2.0, 118.0, CAL_HINT, true);
    draw_text_cap(cr, f.small, 11.0,
                  is_volume ? "wpctl or pactl is needed"
                            : "brightnessctl is needed",
                  width / 2.0, 140.0, CAL_DIM, true);
  } else {
    /* icon + slider */
    const double x0 = LP_SLIDER_X0;
    const double x1 = lp_slider_x1();
    const double cy = LP_SLIDER_CY;
    const double frac = percent / 100.0;
    if (is_volume) {
      draw_speaker(cr, 38.0, cy, fill);
    } else {
      draw_brightness(cr, 38.0, cy, fill, config->background);
    }
    draw_meter(cr, x0, cy - 4.0, x1 - x0, 8.0, frac, fill, config->segment);

    const bool grabbed = ui->dragging || ui->hover_kind == LEVELPOPUP_HIT_SLIDER;
    const double kx = x0 + (x1 - x0) * frac;
    if (grabbed) { /* soft halo while hovered or dragged */
      set_source_hex_a(cr, fill, 0.28);
      cairo_new_path(cr);
      cairo_arc(cr, kx, cy, 13.0, 0.0, 2.0 * M_PI);
      cairo_fill(cr);
    }
    set_source_hex(cr, fg);
    cairo_new_path(cr);
    cairo_arc(cr, kx, cy, 8.0, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
    set_source_hex(cr, fill);
    cairo_new_path(cr);
    cairo_arc(cr, kx, cy, 3.4, 0.0, 2.0 * M_PI);
    cairo_fill(cr);

    /* preset buttons: the one that equals the current value is lit */
    const int *presets = is_volume ? LP_VOLUME_PRESETS : LP_BRIGHTNESS_PRESETS;
    const double bw = lp_preset_width();
    for (int i = 0; i < LP_PRESETS; i++) {
      const double bx = SP_PAD + i * (bw + LP_PRESET_GAP);
      const bool active = presets[i] == percent;
      const bool hover = ui->hover_kind == LEVELPOPUP_HIT_PRESET &&
                         ui->hover_preset == i;

      rounded_rect(cr, bx, LP_PRESET_Y, bw, LP_PRESET_H, 8.0);
      set_source_hex(cr, active ? fill : hover ? "#2d3437" : config->segment);
      cairo_fill(cr);

      snprintf(text, sizeof(text), "%d%%", presets[i]);
      draw_text_cap(cr, f.small_bold, 11.0, text, bx + bw / 2.0,
                    LP_PRESET_Y + LP_PRESET_H / 2.0, active ? config->background : fg,
                    true);
    }

    /* volume only: mute row, the whole row is the button */
    if (is_volume) {
      sys_rule(cr, config, width, LP_RULE_Y);
      const bool hover = ui->hover_kind == LEVELPOPUP_HIT_MUTE;
      if (hover) {
        rounded_rect(cr, 10.0, LP_MUTE_ROW_Y, width - 20.0, LP_MUTE_ROW_H, 8.0);
        set_source_hex(cr, config->segment);
        cairo_fill(cr);
      }
      const double row_cy = LP_MUTE_ROW_Y + LP_MUTE_ROW_H / 2.0;
      draw_text_cap(cr, f.row, 13.0, "Mute", SP_PAD, row_cy, fg, false);
      draw_switch_at(cr, width - SP_PAD - NP_TOGGLE_W, row_cy - NP_TOGGLE_H / 2.0,
                     muted, hover, config);
    }
  }

  /* footer */
  const double footer_sep = lp_footer_sep(is_volume);
  sys_rule(cr, config, width, footer_sep);
  draw_text_cap(cr, f.small, 11.0,
                is_volume ? "Scroll, drag or \xe2\x86\x90\xe2\x86\x92 adjust      M  mute"
                          : "Scroll, drag or \xe2\x86\x90\xe2\x86\x92 adjust",
                SP_PAD, footer_sep + 20.0, CAL_HINT, false);

  sys_fonts_free(&f);
  cairo_destroy(cr);
  cairo_surface_flush(buffer->surface);
}

void render_volume_popup(bar_buffer_t *buffer, const bar_config_t *config,
                         int percent, bool muted, const levelpopup_ui_t *ui) {
  if (buffer == NULL || config == NULL || ui == NULL) {
    return;
  }
  render_level_popup(buffer, config, true, percent, muted, ui);
}

void render_brightness_popup(bar_buffer_t *buffer, const bar_config_t *config,
                             int percent, const levelpopup_ui_t *ui) {
  if (buffer == NULL || config == NULL || ui == NULL) {
    return;
  }
  render_level_popup(buffer, config, false, percent, false, ui);
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
