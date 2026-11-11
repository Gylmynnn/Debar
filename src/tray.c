#define _GNU_SOURCE
#include "tray.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef DEBAR_HAVE_GDK_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#define TRAY_ICON_LOAD_SIZE 48
#define TRAY_THEME_QUEUE_MAX 24

#define SNI_WATCHER "org.kde.StatusNotifierWatcher"
#define SNI_WATCHER_PATH "/StatusNotifierWatcher"
#define SNI_INTERFACE "org.kde.StatusNotifierItem"

static int g_tray_debug = -1;
static void tlog(const char *fmt, ...) {
  va_list ap;
  if (g_tray_debug < 0) {
    g_tray_debug = getenv("DEBAR_DEBUG") != NULL;
  }
  if (g_tray_debug) {
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
  }
}

typedef struct tray_item {
  char *spec;    /* watcher entry, e.g. ":1.49/org/blueman/sni" */
  char *service; /* ":1.49" or a well-known name */
  char *owner;   /* unique bus name currently owning service */
  char *path;    /* "/org/blueman/sni" */
  char *theme_path;
  cairo_surface_t *icon;
  guint props_sub;
  guint item_sub;
  bool ready;  /* properties were read (or the read failed) at least once */
  bool hidden; /* Wi-Fi / Bluetooth applet: the bar has its own popups */
} tray_item_t;

struct tray {
  GMutex mutex;
  tray_item_t items[DEBAR_TRAY_MAX_ITEMS];
  int count;

  int wake_read;
  int wake_write;

  GMainContext *context;
  GMainLoop *loop;
  GThread *thread;
  GDBusConnection *connection;
  gint stop;
};

/* ---------------- small helpers ------------------------------------------ */

static void tray_notify(tray_t *t) {
  if (t->wake_write >= 0) {
    const char byte = 1;
    const ssize_t ignored = write(t->wake_write, &byte, 1);
    (void)ignored;
  }
}

static bool parse_spec(const char *spec, char **service_out, char **path_out) {
  if (spec == NULL || spec[0] == '\0') {
    return false;
  }

  const char *slash = strchr(spec, '/');
  if (slash != NULL) {
    *service_out = g_strndup(spec, (gsize)(slash - spec));
    *path_out = g_strdup(slash);
  } else {
    *service_out = g_strdup(spec);
    *path_out = g_strdup("/StatusNotifierItem");
  }

  if ((*service_out)[0] == '\0' || (*path_out)[0] == '\0') {
    g_free(*service_out);
    g_free(*path_out);
    *service_out = NULL;
    *path_out = NULL;
    return false;
  }
  return true;
}

static int find_item_exact_locked(tray_t *t, const char *service,
                                  const char *path) {
  for (int i = 0; i < t->count; i++) {
    if (g_strcmp0(t->items[i].service, service) == 0 &&
        g_strcmp0(t->items[i].path, path) == 0) {
      return i;
    }
  }
  return -1;
}

/* Signals always arrive with the sender's unique name, which can differ from
 * the well-known name the watcher registered. Match the stored owner first;
 * the path alone is not enough because many items use /StatusNotifierItem. */
static int find_item_for_signal_locked(tray_t *t, const char *sender,
                                       const char *path) {
  for (int i = 0; i < t->count; i++) {
    if (g_strcmp0(t->items[i].path, path) == 0 &&
        g_strcmp0(t->items[i].owner, sender) == 0) {
      return i;
    }
  }
  for (int i = 0; i < t->count; i++) {
    if (g_strcmp0(t->items[i].path, path) == 0 &&
        g_strcmp0(t->items[i].service, sender) == 0) {
      return i;
    }
  }
  return -1;
}

/* ---------------- hidden items (Wi-Fi / Bluetooth) ----------------------- */

/* The bar has its own Wi-Fi and Bluetooth popups, so the applets that would
 * show the same thing in the tray (nm-applet, blueman, ...) are kept out of
 * it. They stay in the item list so D-Bus signals and removal keep working;
 * they are only skipped when the tray is counted, drawn or clicked. */
static const char *const HIDDEN_NAME_PARTS[] = {
    "blueman",    "bluetooth", "blueberry", "bluedevil", "nm-applet",
    "nm_applet",  "nm-tray",   "nm-connection", "networkmanager",
    "network-manager", "network-wireless",
};

/* Icon names NetworkManager applets use for their state ("nm-signal-75",
 * "nm-no-connection", "network-offline", ...). */
static const char *const HIDDEN_ICON_PREFIXES[] = {
    "nm-", "network-wireless", "network-wired", "network-offline",
    "network-idle", "network-transmit", "network-error", "network-no-route",
};

static bool contains_icase(const char *haystack, const char *needle) {
  if (haystack == NULL || haystack[0] == '\0') {
    return false;
  }
  char *h = g_ascii_strdown(haystack, -1);
  const bool found = strstr(h, needle) != NULL;
  g_free(h);
  return found;
}

/* Decides from the item's own properties (Id, Title, IconName) and from its
 * D-Bus name/path. */
static bool item_is_hidden(GVariant *properties, const char *service,
                           const char *path) {
  const char *fields[3] = {NULL, NULL, NULL};
  (void)g_variant_lookup(properties, "Id", "&s", &fields[0]);
  (void)g_variant_lookup(properties, "Title", "&s", &fields[1]);
  (void)g_variant_lookup(properties, "IconName", "&s", &fields[2]);

  for (size_t i = 0; i < G_N_ELEMENTS(HIDDEN_NAME_PARTS); i++) {
    const char *part = HIDDEN_NAME_PARTS[i];
    if (contains_icase(fields[0], part) || contains_icase(fields[1], part) ||
        contains_icase(fields[2], part) || contains_icase(path, part) ||
        contains_icase(service, part)) {
      return true;
    }
  }

  if (fields[2] != NULL) {
    char *icon = g_ascii_strdown(fields[2], -1);
    bool hide = false;
    for (size_t i = 0; i < G_N_ELEMENTS(HIDDEN_ICON_PREFIXES) && !hide; i++) {
      hide = g_str_has_prefix(icon, HIDDEN_ICON_PREFIXES[i]);
    }
    g_free(icon);
    if (hide) {
      return true;
    }
  }
  return false;
}

static bool item_visible(const tray_item_t *item) {
  return item->ready && !item->hidden;
}

/* Maps the index among the visible items to the index in items[], or -1. */
static int visible_to_real_locked(const tray_t *t, int visible) {
  if (visible < 0) {
    return -1;
  }
  int seen = 0;
  for (int i = 0; i < t->count; i++) {
    if (!item_visible(&t->items[i])) {
      continue;
    }
    if (seen == visible) {
      return i;
    }
    seen++;
  }
  return -1;
}

/* ---------------- icon pixmaps ------------------------------------------- */

static int pixmap_score(int width, int height) {
  const int m = width > height ? width : height;
  if (m >= 20) {
    return 2000 - (m - 20); /* prefer the smallest size >= 20 */
  }
  return m; /* nothing large enough: prefer the biggest small one */
}

static cairo_surface_t *surface_from_pixmaps(GVariant *pixmaps) {
  if (pixmaps == NULL || g_variant_n_children(pixmaps) == 0) {
    return NULL;
  }

  GVariantIter iter;
  g_variant_iter_init(&iter, pixmaps);

  gint width = 0;
  gint height = 0;
  GVariant *data = NULL;
  GBytes *best = NULL;
  int best_width = 0;
  int best_height = 0;
  int best_score = -1;

  while (g_variant_iter_next(&iter, "(ii@ay)", &width, &height, &data)) {
    gsize length = 0;
    const guint8 *bytes = g_variant_get_fixed_array(data, &length, 1);
    if (width > 0 && height > 0 &&
        length == (gsize)width * (gsize)height * 4u) {
      const int score = pixmap_score(width, height);
      if (score > best_score) {
        g_clear_pointer(&best, g_bytes_unref);
        best = g_bytes_new(bytes, length);
        best_width = width;
        best_height = height;
        best_score = score;
      }
    }
    g_variant_unref(data);
    data = NULL;
  }

  if (best == NULL) {
    return NULL;
  }

  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_width, best_height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    g_bytes_unref(best);
    return NULL;
  }

  gsize length = 0;
  const guint8 *src = g_bytes_get_data(best, &length);
  guint32 *dst = (guint32 *)cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface) / 4;

  /* StatusNotifierItem pixmaps are non-premultiplied ARGB32 in network byte
   * order: A, R, G, B. Cairo wants premultiplied native-endian ARGB32. */
  for (int y = 0; y < best_height; y++) {
    for (int x = 0; x < best_width; x++) {
      const guint8 *p = src + ((gsize)y * (gsize)best_width + (gsize)x) * 4u;
      const guint32 a = p[0];
      const guint32 r = (guint32)p[1] * a / 255u;
      const guint32 g = (guint32)p[2] * a / 255u;
      const guint32 b = (guint32)p[3] * a / 255u;
      dst[(gsize)y * (gsize)stride + (gsize)x] =
          (a << 24) | (r << 16) | (g << 8) | b;
    }
  }

  cairo_surface_mark_dirty(surface);
  g_bytes_unref(best);
  return surface;
}

/* ---------------- icon theme lookup -------------------------------------- */

typedef struct theme_dir {
  char *rel;
  int size;
  bool scalable;
  bool symbolic;
} theme_dir_t;

typedef struct theme_dirs {
  theme_dir_t *items;
  int count;
  int capacity;
} theme_dirs_t;

static void theme_dirs_add(theme_dirs_t *dirs, const char *rel, int size,
                           bool scalable, bool symbolic) {
  if (rel == NULL || rel[0] == '\0') {
    return;
  }
  if (dirs->count == dirs->capacity) {
    dirs->capacity = dirs->capacity == 0 ? 16 : dirs->capacity * 2;
    dirs->items =
        g_realloc(dirs->items, (gsize)dirs->capacity * sizeof(*dirs->items));
  }
  theme_dir_t *entry = &dirs->items[dirs->count++];
  entry->rel = g_strdup(rel);
  entry->size = size;
  entry->scalable = scalable;
  entry->symbolic = symbolic;
}

static void theme_dirs_clear(theme_dirs_t *dirs) {
  for (int i = 0; i < dirs->count; i++) {
    g_free(dirs->items[i].rel);
  }
  g_free(dirs->items);
  memset(dirs, 0, sizeof(*dirs));
}

static int theme_dir_score(const theme_dir_t *dir) {
  if (dir->symbolic) {
    return -10000; /* symbolic icons need recolouring: use last */
  }
  if (dir->scalable) {
    return 1990;
  }
  if (dir->size >= 20) {
    return 2000 - (dir->size - 20);
  }
  return dir->size;
}

static int theme_dir_compare(const void *a, const void *b) {
  const theme_dir_t *da = a;
  const theme_dir_t *db = b;
  const int sa = theme_dir_score(da);
  const int sb = theme_dir_score(db);
  return sa == sb ? 0 : (sa > sb ? -1 : 1);
}

static int size_from_rel(const char *rel) {
  for (const char *p = rel; *p != '\0'; p++) {
    if (*p >= '0' && *p <= '9') {
      return atoi(p);
    }
  }
  return 0;
}

/* Icon theme index files use commas, while GKeyFile's list helpers default
 * to semicolons. Split the raw value ourselves. */
static char **key_file_comma_list(GKeyFile *key_file, const char *group,
                                  const char *key, gsize *n_out) {
  char *raw = g_key_file_get_string(key_file, group, key, NULL);
  if (raw == NULL) {
    return NULL;
  }
  char **list = g_strsplit(raw, ",", -1);
  g_free(raw);
  if (n_out != NULL) {
    *n_out = g_strv_length(list);
  }
  return list;
}

static void add_index_dirs(theme_dirs_t *dirs, GKeyFile *key_file,
                           const char *key) {
  gsize n = 0;
  char **list = key_file_comma_list(key_file, "Icon Theme", key, &n);
  if (list == NULL) {
    return;
  }
  for (gsize i = 0; i < n; i++) {
    g_strstrip(list[i]);
    const char *rel = list[i];
    if (rel[0] == '\0') {
      continue;
    }
    bool scalable = strstr(rel, "scalable") != NULL;
    if (!scalable && g_key_file_has_key(key_file, rel, "Type", NULL)) {
      char *type = g_key_file_get_string(key_file, rel, "Type", NULL);
      scalable = g_strcmp0(type, "Scalable") == 0;
      g_free(type);
    }
    const bool symbolic = strstr(rel, "symbolic") != NULL;
    int size = 0;
    if (g_key_file_has_key(key_file, rel, "Size", NULL)) {
      size = g_key_file_get_integer(key_file, rel, "Size", NULL);
    }
    if (size == 0) {
      size = size_from_rel(rel);
    }
    theme_dirs_add(dirs, rel, size, scalable, symbolic);
  }
  g_strfreev(list);
}

static theme_dirs_t read_theme_dirs(const char *theme_path) {
  theme_dirs_t dirs;
  memset(&dirs, 0, sizeof(dirs));

  char *index = g_build_filename(theme_path, "index.theme", NULL);
  GKeyFile *key_file = g_key_file_new();
  if (g_key_file_load_from_file(key_file, index, G_KEY_FILE_NONE, NULL)) {
    add_index_dirs(&dirs, key_file, "Directories");
    add_index_dirs(&dirs, key_file, "ScaledDirectories");
  }
  g_key_file_free(key_file);
  g_free(index);

  if (dirs.count == 0) {
    static const char *fallback[] = {
        "24x24/apps",   "24x24/status",   "22x22/apps",   "22x22/status",
        "16x16/apps",   "16x16/status",   "32x32/apps",   "32x32/status",
        "48x48/apps",   "48x48/status",   "apps/24",      "apps/22",
        "apps/16",      "apps/32",        "apps/48",      "status/24",
        "status/22",    "status/16",      "status/32",    "status/48",
        "scalable/apps", "scalable/status", "apps/scalable", "status/scalable",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(fallback); i++) {
      const char *rel = fallback[i];
      theme_dirs_add(&dirs, rel, size_from_rel(rel),
                     strstr(rel, "scalable") != NULL,
                     strstr(rel, "symbolic") != NULL);
    }
  }

  qsort(dirs.items, (gsize)dirs.count, sizeof(*dirs.items), theme_dir_compare);
  return dirs;
}

static char *find_theme_path(const char *base, const char *name) {
  char *path = g_build_filename(base, name, NULL);
  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    return path;
  }
  g_free(path);

  char *normalized = g_ascii_strdown(name, -1);
  for (char *p = normalized; *p != '\0'; p++) {
    if (*p == ' ') {
      *p = '-';
    }
  }
  path = g_build_filename(base, normalized, NULL);
  g_free(normalized);
  if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
    return path;
  }
  g_free(path);

  /* Some themes ship a directory name that differs from their Name= field
   * ("Breeze Dark" -> breeze-dark). Match the index.theme name too. */
  GDir *dir = g_dir_open(base, 0, NULL);
  if (dir == NULL) {
    return NULL;
  }

  char *found = NULL;
  const char *entry = NULL;
  while ((entry = g_dir_read_name(dir)) != NULL) {
    char *index = g_build_filename(base, entry, "index.theme", NULL);
    GKeyFile *key_file = g_key_file_new();
    if (g_key_file_load_from_file(key_file, index, G_KEY_FILE_NONE, NULL)) {
      char *theme_name =
          g_key_file_get_string(key_file, "Icon Theme", "Name", NULL);
      if (theme_name != NULL && g_ascii_strcasecmp(theme_name, name) == 0) {
        found = g_build_filename(base, entry, NULL);
      }
      g_free(theme_name);
    }
    g_key_file_free(key_file);
    g_free(index);
    if (found != NULL) {
      break;
    }
  }
  g_dir_close(dir);
  return found;
}

static char **read_inherits(const char *theme_path) {
  char *index = g_build_filename(theme_path, "index.theme", NULL);
  GKeyFile *key_file = g_key_file_new();
  char **list = NULL;
  if (g_key_file_load_from_file(key_file, index, G_KEY_FILE_NONE, NULL)) {
    gsize n = 0;
    list = key_file_comma_list(key_file, "Icon Theme", "Inherits", &n);
    if (list != NULL) {
      for (gsize i = 0; i < n; i++) {
        g_strstrip(list[i]);
      }
    }
  }
  g_key_file_free(key_file);
  g_free(index);
  return list;
}

static char *icon_in_theme(const char *theme_path, const char *name) {
  theme_dirs_t dirs = read_theme_dirs(theme_path);

  static const char *suffixes[] = {"", "-symbolic"};
  char *found = NULL;

  for (gsize s = 0; s < G_N_ELEMENTS(suffixes) && found == NULL; s++) {
    for (int i = 0; i < dirs.count && found == NULL; i++) {
      char *base = g_strdup_printf("%s/%s/%s%s", theme_path,
                                   dirs.items[i].rel, name, suffixes[s]);
      char *png = g_strconcat(base, ".png", NULL);
      char *svg = g_strconcat(base, ".svg", NULL);
      g_free(base);

      if (g_file_test(png, G_FILE_TEST_IS_REGULAR)) {
        found = png;
        g_free(svg);
      } else {
        g_free(png);
#ifdef DEBAR_HAVE_GDK_PIXBUF
        if (g_file_test(svg, G_FILE_TEST_IS_REGULAR)) {
          found = svg;
        } else {
          g_free(svg);
        }
#else
        g_free(svg);
#endif
      }
    }
  }

  theme_dirs_clear(&dirs);
  return found;
}

static char *active_icon_theme(void) {
  const char *env = g_getenv("DEBAR_ICON_THEME");
  if (env != NULL && env[0] != '\0') {
    return g_strdup(env);
  }

  const char *config = g_get_user_config_dir();
  static const char *files[] = {"gtk-3.0/settings.ini", "gtk-4.0/settings.ini"};
  for (gsize i = 0; i < G_N_ELEMENTS(files); i++) {
    char *path = g_build_filename(config, files[i], NULL);
    GKeyFile *key_file = g_key_file_new();
    if (g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL)) {
      char *theme =
          g_key_file_get_string(key_file, "Settings", "gtk-icon-theme-name",
                                NULL);
      g_key_file_free(key_file);
      g_free(path);
      if (theme != NULL && theme[0] != '\0') {
        return theme;
      }
      g_free(theme);
    } else {
      g_key_file_free(key_file);
      g_free(path);
    }
  }
  return g_strdup("hicolor");
}

static GPtrArray *icon_bases(void) {
  GPtrArray *bases = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(bases,
                  g_build_filename(g_get_user_data_dir(), "icons", NULL));
  g_ptr_array_add(bases, g_build_filename(g_get_home_dir(), ".icons", NULL));

  const char *const *system = g_get_system_data_dirs();
  for (int i = 0; system != NULL && system[i] != NULL; i++) {
    g_ptr_array_add(bases, g_build_filename(system[i], "icons", NULL));
  }
  return bases;
}

static void theme_queue_push(GPtrArray *queue, GHashTable *seen,
                             const char *name) {
  if (name == NULL || name[0] == '\0' || queue->len >= TRAY_THEME_QUEUE_MAX) {
    return;
  }
  char *key = g_ascii_strdown(name, -1);
  if (g_hash_table_contains(seen, key)) {
    g_free(key);
    return;
  }
  g_hash_table_add(seen, key);
  g_ptr_array_add(queue, g_strdup(name));
}

static char *find_icon_file(const char *name, const char *extra_theme_path) {
  if (name == NULL || name[0] == '\0') {
    return NULL;
  }

  if (name[0] == '/' && g_file_test(name, G_FILE_TEST_IS_REGULAR)) {
    return g_strdup(name);
  }

  if (extra_theme_path != NULL && extra_theme_path[0] != '\0') {
    char *found = icon_in_theme(extra_theme_path, name);
    if (found != NULL) {
      return found;
    }
    static const char *exts[] = {".png", ".svg"};
    for (gsize i = 0; i < G_N_ELEMENTS(exts); i++) {
      char *file = g_strconcat(extra_theme_path, "/", name, exts[i], NULL);
      if (g_file_test(file, G_FILE_TEST_IS_REGULAR)) {
        return file;
      }
      g_free(file);
    }
  }

  GPtrArray *bases = icon_bases();
  GPtrArray *queue = g_ptr_array_new_with_free_func(g_free);
  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  char *theme = active_icon_theme();
  theme_queue_push(queue, seen, theme);
  theme_queue_push(queue, seen, "hicolor");
  g_free(theme);

  char *found = NULL;
  for (guint qi = 0; qi < queue->len && found == NULL; qi++) {
    const char *theme_name = g_ptr_array_index(queue, qi);
    for (guint bi = 0; bi < bases->len && found == NULL; bi++) {
      const char *base = g_ptr_array_index(bases, bi);
      char *theme_path = find_theme_path(base, theme_name);
      if (theme_path == NULL) {
        continue;
      }

      found = icon_in_theme(theme_path, name);
      if (found == NULL) {
        char **inherits = read_inherits(theme_path);
        if (inherits != NULL) {
          for (gsize i = 0; inherits[i] != NULL; i++) {
            theme_queue_push(queue, seen, inherits[i]);
          }
          g_strfreev(inherits);
        }
      }
      g_free(theme_path);
    }
  }

  g_hash_table_destroy(seen);
  g_ptr_array_free(queue, TRUE);
  g_ptr_array_free(bases, TRUE);

  if (found != NULL) {
    return found;
  }

  /* Last resort: flat pixmap directories. */
  GPtrArray *pixmaps = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(pixmaps,
                  g_build_filename(g_get_user_data_dir(), "pixmaps", NULL));
  g_ptr_array_add(pixmaps, g_strdup("/usr/share/pixmaps"));

  static const char *exts[] = {".png", ".svg"};
  for (guint bi = 0; bi < pixmaps->len && found == NULL; bi++) {
    const char *base = g_ptr_array_index(pixmaps, bi);
    for (gsize i = 0; i < G_N_ELEMENTS(exts); i++) {
      char *file = g_strconcat(base, "/", name, exts[i], NULL);
      if (g_file_test(file, G_FILE_TEST_IS_REGULAR)) {
        found = file;
        break;
      }
      g_free(file);
    }
  }
  g_ptr_array_free(pixmaps, TRUE);
  return found;
}

#ifdef DEBAR_HAVE_GDK_PIXBUF
static cairo_surface_t *surface_from_pixbuf(GdkPixbuf *pixbuf) {
  const int width = gdk_pixbuf_get_width(pixbuf);
  const int height = gdk_pixbuf_get_height(pixbuf);
  const int channels = gdk_pixbuf_get_n_channels(pixbuf);
  const int stride = gdk_pixbuf_get_rowstride(pixbuf);
  const guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);

  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return NULL;
  }

  guint32 *dst = (guint32 *)cairo_image_surface_get_data(surface);
  const int dst_stride = cairo_image_surface_get_stride(surface) / 4;

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const guchar *p = pixels + (gsize)y * (gsize)stride +
                        (gsize)x * (gsize)channels;
      const guint32 a = channels == 4 ? p[3] : 255u;
      const guint32 r = (guint32)p[0] * a / 255u;
      const guint32 g = (guint32)p[1] * a / 255u;
      const guint32 b = (guint32)p[2] * a / 255u;
      dst[(gsize)y * (gsize)dst_stride + (gsize)x] =
          (a << 24) | (r << 16) | (g << 8) | b;
    }
  }

  cairo_surface_mark_dirty(surface);
  return surface;
}
#endif

static cairo_surface_t *load_icon_file(const char *path) {
  if (path == NULL) {
    return NULL;
  }

#ifdef DEBAR_HAVE_GDK_PIXBUF
  GError *error = NULL;
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
      path, TRAY_ICON_LOAD_SIZE, TRAY_ICON_LOAD_SIZE, TRUE, &error);
  if (pixbuf == NULL) {
    tlog("debar tray: cannot load %s: %s", path,
         error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    return NULL;
  }
  cairo_surface_t *surface = surface_from_pixbuf(pixbuf);
  g_object_unref(pixbuf);
  return surface;
#else
  if (!g_str_has_suffix(path, ".png")) {
    return NULL;
  }
  cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return NULL;
  }
  return surface;
#endif
}

static cairo_surface_t *surface_from_icon_name(const char *name,
                                               const char *theme_path) {
  char *file = find_icon_file(name, theme_path);
  if (file == NULL) {
    tlog("debar tray: icon '%s' not found", name);
    return NULL;
  }

  cairo_surface_t *surface = load_icon_file(file);
  tlog("debar tray: icon '%s' -> %s (%s)", name, file,
       surface != NULL ? "ok" : "failed");
  g_free(file);
  return surface;
}

static cairo_surface_t *surface_from_prop_pixmaps(GVariant *properties,
                                                  const char *key) {
  GVariant *pixmaps = NULL;
  if (!g_variant_lookup(properties, key, "@a(iiay)", &pixmaps)) {
    return NULL;
  }
  cairo_surface_t *surface = surface_from_pixmaps(pixmaps);
  g_variant_unref(pixmaps);
  return surface;
}

static cairo_surface_t *icon_from_properties(GVariant *properties,
                                             const char *theme_path) {
  const char *status = NULL;
  const bool attention =
      g_variant_lookup(properties, "Status", "&s", &status) &&
      g_strcmp0(status, "NeedsAttention") == 0;

  cairo_surface_t *surface = NULL;
  if (attention) {
    surface = surface_from_prop_pixmaps(properties, "AttentionIconPixmap");
  }
  if (surface == NULL) {
    surface = surface_from_prop_pixmaps(properties, "IconPixmap");
  }
  if (surface != NULL) {
    return surface;
  }

  const char *name = NULL;
  if (attention &&
      g_variant_lookup(properties, "AttentionIconName", "&s", &name) &&
      name[0] != '\0') {
    surface = surface_from_icon_name(name, theme_path);
  }
  if (surface == NULL &&
      g_variant_lookup(properties, "IconName", "&s", &name) &&
      name[0] != '\0') {
    surface = surface_from_icon_name(name, theme_path);
  }
  return surface;
}

/* Icons are drawn at ~22 px. Apps often send 128-512 px pixmaps; keeping them
 * as they are wastes up to a megabyte each and makes every frame rescale a
 * huge source, so anything bigger than TRAY_ICON_LOAD_SIZE is shrunk once. */
static cairo_surface_t *limit_icon_size(cairo_surface_t *icon) {
  if (icon == NULL) {
    return NULL;
  }
  const int w = cairo_image_surface_get_width(icon);
  const int h = cairo_image_surface_get_height(icon);
  const int longest = w > h ? w : h;
  if (w <= 0 || h <= 0 || longest <= TRAY_ICON_LOAD_SIZE) {
    return icon;
  }

  const double scale = (double)TRAY_ICON_LOAD_SIZE / (double)longest;
  int nw = (int)((double)w * scale + 0.5);
  int nh = (int)((double)h * scale + 0.5);
  nw = nw < 1 ? 1 : nw;
  nh = nh < 1 ? 1 : nh;

  cairo_surface_t *small =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nw, nh);
  if (cairo_surface_status(small) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(small);
    return icon;
  }

  cairo_t *cr = cairo_create(small);
  cairo_scale(cr, (double)nw / (double)w, (double)nh / (double)h);
  cairo_set_source_surface(cr, icon, 0.0, 0.0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_paint(cr);
  cairo_destroy(cr);

  cairo_surface_destroy(icon);
  return small;
}

/* Icon lookup parses theme files and decodes images, which leaves freed but
 * unreturned heap behind; hand it back to the OS. */
static void trim_heap(void) {
#ifdef __GLIBC__
  malloc_trim(0);
#endif
}

/* ---------------- D-Bus item handling ------------------------------------ */

static char *resolve_owner(GDBusConnection *connection, const char *service) {
  if (service[0] == ':') {
    return g_strdup(service);
  }

  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(
      connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", service),
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
  if (reply == NULL) {
    g_clear_error(&error);
    return NULL;
  }

  const char *owner = NULL;
  g_variant_get(reply, "(&s)", &owner);
  char *copy = owner != NULL ? g_strdup(owner) : NULL;
  g_variant_unref(reply);
  return copy;
}

static void refresh_item(tray_t *t, const char *service, const char *path) {
  GDBusConnection *connection = NULL;
  g_mutex_lock(&t->mutex);
  if (t->connection != NULL) {
    connection = g_object_ref(t->connection);
  }
  g_mutex_unlock(&t->mutex);
  if (connection == NULL) {
    return;
  }

  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(
      connection, service, path, "org.freedesktop.DBus.Properties", "GetAll",
      g_variant_new("(s)", SNI_INTERFACE), G_VARIANT_TYPE("(a{sv})"),
      G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
  if (reply == NULL) {
    tlog("debar tray: cannot read properties of %s%s: %s", service, path,
         error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    g_object_unref(connection);

    /* An item that cannot be read still gets its (empty) slot, as before. */
    bool shown = false;
    g_mutex_lock(&t->mutex);
    int failed = find_item_exact_locked(t, service, path);
    if (failed < 0) {
      failed = find_item_for_signal_locked(t, service, path);
    }
    if (failed >= 0 && !t->items[failed].ready) {
      t->items[failed].ready = true;
      shown = true;
    }
    g_mutex_unlock(&t->mutex);
    if (shown) {
      tray_notify(t);
    }
    return;
  }

  GVariant *properties = NULL;
  g_variant_get(reply, "(@a{sv})", &properties);

  const char *theme_path = NULL;
  char *theme_copy = NULL;
  if (g_variant_lookup(properties, "IconThemePath", "&s", &theme_path) &&
      theme_path[0] != '\0') {
    theme_copy = g_strdup(theme_path);
  }

  cairo_surface_t *icon =
      limit_icon_size(icon_from_properties(properties, theme_copy));
  const bool hidden = item_is_hidden(properties, service, path);

  g_variant_unref(properties);
  g_variant_unref(reply);
  g_object_unref(connection);

  bool changed = false;
  g_mutex_lock(&t->mutex);
  int index = find_item_exact_locked(t, service, path);
  if (index < 0) {
    index = find_item_for_signal_locked(t, service, path);
  }
  if (index >= 0) {
    tray_item_t *item = &t->items[index];
    if (hidden != item->hidden || !item->ready) {
      tlog("debar tray: %s%s is %s", service, path,
           hidden ? "hidden (Wi-Fi/Bluetooth applet)" : "shown");
    }
    item->hidden = hidden;
    item->ready = true;
    if (icon != NULL) {
      if (item->icon != NULL) {
        cairo_surface_destroy(item->icon);
      }
      item->icon = icon;
      icon = NULL;
    }
    if (theme_copy != NULL) {
      g_free(item->theme_path);
      item->theme_path = theme_copy;
      theme_copy = NULL;
    }
    changed = true;
  }
  g_mutex_unlock(&t->mutex);

  if (icon != NULL) {
    cairo_surface_destroy(icon);
  }
  g_free(theme_copy);

  if (changed) {
    tray_notify(t);
  }
  trim_heap();
}

static void on_item_signal(GDBusConnection *connection,
                           const gchar *sender_name, const gchar *object_path,
                           const gchar *interface_name,
                           const gchar *signal_name, GVariant *parameters,
                           gpointer user_data) {
  (void)connection;
  (void)interface_name;
  (void)signal_name;
  (void)parameters;
  tray_t *t = user_data;

  /* Subscriptions match any sender (the watcher and several items announce
   * themselves through unique names), so ignore unrelated paths. */
  g_mutex_lock(&t->mutex);
  const bool known =
      find_item_for_signal_locked(t, sender_name, object_path) >= 0;
  g_mutex_unlock(&t->mutex);
  if (known) {
    refresh_item(t, sender_name, object_path);
  }
}

static void add_item(tray_t *t, const char *spec) {
  char *service = NULL;
  char *path = NULL;
  if (!parse_spec(spec, &service, &path)) {
    return;
  }

  GDBusConnection *connection = NULL;
  g_mutex_lock(&t->mutex);
  if (t->connection != NULL) {
    connection = g_object_ref(t->connection);
  }
  g_mutex_unlock(&t->mutex);
  if (connection == NULL) {
    g_free(service);
    g_free(path);
    return;
  }

  /* Signals use the sender's unique name, so remember the current owner of
   * a well-known service as well. */
  char *owner = resolve_owner(connection, service);
  const bool owner_resolved = owner != NULL;
  if (owner == NULL) {
    owner = g_strdup(service);
  }

  g_mutex_lock(&t->mutex);
  const int existing = find_item_exact_locked(t, service, path);
  if (existing >= 0) {
    /* The same item re-registered (for example after its app restarted):
     * refresh the owner so later signals still match. */
    if (owner_resolved) {
      g_free(t->items[existing].owner);
      t->items[existing].owner = owner;
      owner = NULL;
    } else {
      g_free(owner);
      owner = NULL;
    }
    char *refresh_service = g_strdup(t->items[existing].service);
    char *refresh_path = g_strdup(t->items[existing].path);
    g_mutex_unlock(&t->mutex);
    g_object_unref(connection);
    g_free(service);
    g_free(path);
    refresh_item(t, refresh_service, refresh_path);
    g_free(refresh_service);
    g_free(refresh_path);
    return;
  }

  if (t->count >= DEBAR_TRAY_MAX_ITEMS) {
    g_mutex_unlock(&t->mutex);
    g_object_unref(connection);
    g_free(service);
    g_free(owner);
    g_free(path);
    return;
  }

  tray_item_t *item = &t->items[t->count];
  memset(item, 0, sizeof(*item));
  item->spec = g_strdup(spec);
  item->service = service;
  item->owner = owner;
  item->path = path;
  item->props_sub = g_dbus_connection_signal_subscribe(
      t->connection, NULL, "org.freedesktop.DBus.Properties",
      "PropertiesChanged", item->path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
      on_item_signal, t, NULL);
  item->item_sub = g_dbus_connection_signal_subscribe(
      t->connection, NULL, SNI_INTERFACE, NULL, item->path, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, on_item_signal, t, NULL);
  t->count++;
  g_mutex_unlock(&t->mutex);
  g_object_unref(connection);

  tlog("debar tray: added %s (owner %s)", spec, owner);
  tray_notify(t);

  char *refresh_service = g_strdup(service);
  char *refresh_path = g_strdup(path);
  refresh_item(t, refresh_service, refresh_path);
  g_free(refresh_service);
  g_free(refresh_path);
}

static void remove_item(tray_t *t, const char *spec) {
  g_mutex_lock(&t->mutex);
  int index = -1;
  for (int i = 0; i < t->count; i++) {
    if (g_strcmp0(t->items[i].spec, spec) == 0 ||
        g_strcmp0(t->items[i].service, spec) == 0) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    g_mutex_unlock(&t->mutex);
    return;
  }

  tray_item_t *item = &t->items[index];
  if (t->connection != NULL) {
    if (item->props_sub != 0) {
      g_dbus_connection_signal_unsubscribe(t->connection, item->props_sub);
    }
    if (item->item_sub != 0) {
      g_dbus_connection_signal_unsubscribe(t->connection, item->item_sub);
    }
  }

  g_free(item->spec);
  g_free(item->service);
  g_free(item->owner);
  g_free(item->path);
  g_free(item->theme_path);
  if (item->icon != NULL) {
    cairo_surface_destroy(item->icon);
  }

  for (int i = index; i + 1 < t->count; i++) {
    t->items[i] = t->items[i + 1];
  }
  t->count--;
  memset(&t->items[t->count], 0, sizeof(t->items[t->count]));
  g_mutex_unlock(&t->mutex);

  tlog("debar tray: removed %s", spec);
  tray_notify(t);
}

static void on_watcher_signal(GDBusConnection *connection,
                              const gchar *sender_name,
                              const gchar *object_path,
                              const gchar *interface_name,
                              const gchar *signal_name,
                              GVariant *parameters, gpointer user_data) {
  (void)connection;
  (void)sender_name;
  (void)object_path;
  (void)interface_name;
  tray_t *t = user_data;

  const char *spec = NULL;
  g_variant_get(parameters, "(&s)", &spec);
  tlog("debar tray: watcher signal %s %s", signal_name, spec);
  if (g_strcmp0(signal_name, "StatusNotifierItemRegistered") == 0) {
    add_item(t, spec);
  } else if (g_strcmp0(signal_name, "StatusNotifierItemUnregistered") == 0) {
    remove_item(t, spec);
  }
}

static void load_initial_items(tray_t *t, GDBusConnection *connection) {
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(
      connection, SNI_WATCHER, SNI_WATCHER_PATH,
      "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", SNI_WATCHER, "RegisteredStatusNotifierItems"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
  if (reply == NULL) {
    tlog("debar tray: cannot list items: %s",
         error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    return;
  }

  GVariant *value = NULL;
  g_variant_get(reply, "(v)", &value);
  if (value != NULL && g_variant_is_of_type(value, G_VARIANT_TYPE("as"))) {
    GVariantIter iter;
    g_variant_iter_init(&iter, value);
    const char *spec = NULL;
    while (g_variant_iter_next(&iter, "&s", &spec)) {
      add_item(t, spec);
    }
  }
  if (value != NULL) {
    g_variant_unref(value);
  }
  g_variant_unref(reply);
}

/* ---------------- thread / lifetime -------------------------------------- */

static gboolean stop_timeout(gpointer data) {
  tray_t *t = data;
  if (g_atomic_int_get(&t->stop)) {
    g_main_loop_quit(t->loop);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gpointer tray_thread(gpointer data) {
  tray_t *t = data;
  g_main_context_push_thread_default(t->context);

  GError *error = NULL;
  char *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL,
                                                  &error);
  if (address == NULL) {
    tlog("debar tray: cannot resolve the session bus: %s",
         error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    g_main_context_pop_thread_default(t->context);
    return NULL;
  }

  /* A private connection is essential: the shared g_bus_get_sync() object
   * may have been created with the global main context, where our tray
   * thread would never receive signals. This one inherits the thread-default
   * context pushed above. */
  GDBusConnection *connection = g_dbus_connection_new_for_address_sync(
      address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  g_free(address);
  if (connection == NULL) {
    tlog("debar tray: cannot connect to the session bus: %s",
         error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    g_main_context_pop_thread_default(t->context);
    return NULL;
  }
  g_dbus_connection_set_exit_on_close(connection, FALSE);

  g_mutex_lock(&t->mutex);
  t->connection = g_object_ref(connection);
  g_mutex_unlock(&t->mutex);

  /* Match on interface/path/member instead of the well-known sender: some
   * watchers emit from their unique name, which well-known sender matching
   * does not always resolve here. */
  const guint registered_sub = g_dbus_connection_signal_subscribe(
      connection, NULL, SNI_WATCHER, "StatusNotifierItemRegistered",
      SNI_WATCHER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_watcher_signal, t,
      NULL);
  const guint unregistered_sub = g_dbus_connection_signal_subscribe(
      connection, NULL, SNI_WATCHER, "StatusNotifierItemUnregistered",
      SNI_WATCHER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_watcher_signal, t,
      NULL);

  const char *unique = g_dbus_connection_get_unique_name(connection);
  GVariant *reply = g_dbus_connection_call_sync(
      connection, SNI_WATCHER, SNI_WATCHER_PATH, SNI_WATCHER,
      "RegisterStatusNotifierHost", g_variant_new("(s)", unique), NULL,
      G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
  if (reply == NULL) {
    tlog("debar tray: cannot register host: %s",
         error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
  } else {
    g_variant_unref(reply);
  }

  load_initial_items(t, connection);

  GSource *source = g_timeout_source_new(250);
  g_source_set_callback(source, stop_timeout, t, NULL);
  g_source_attach(source, t->context);
  g_source_unref(source);

  tlog("debar tray: ready (%d item%s)", t->count, t->count == 1 ? "" : "s");
  g_main_loop_run(t->loop);

  g_dbus_connection_signal_unsubscribe(connection, registered_sub);
  g_dbus_connection_signal_unsubscribe(connection, unregistered_sub);
  g_object_unref(connection);
  g_main_context_pop_thread_default(t->context);
  return NULL;
}

tray_t *tray_create(void) {
  tray_t *t = g_new0(tray_t, 1);
  g_mutex_init(&t->mutex);
  t->wake_read = -1;
  t->wake_write = -1;

  int fds[2] = {-1, -1};
  if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
    t->wake_read = fds[0];
    t->wake_write = fds[1];
  }

  t->context = g_main_context_new();
  t->loop = g_main_loop_new(t->context, FALSE);
  t->thread = g_thread_new("debar-tray", tray_thread, t);
  return t;
}

void tray_destroy(tray_t *t) {
  if (t == NULL) {
    return;
  }

  g_atomic_int_set(&t->stop, 1);
  if (t->loop != NULL) {
    g_main_loop_quit(t->loop);
  }
  if (t->thread != NULL) {
    g_thread_join(t->thread);
    t->thread = NULL;
  }

  for (int i = 0; i < t->count; i++) {
    tray_item_t *item = &t->items[i];
    g_free(item->spec);
    g_free(item->service);
    g_free(item->owner);
    g_free(item->path);
    g_free(item->theme_path);
    if (item->icon != NULL) {
      cairo_surface_destroy(item->icon);
    }
  }
  t->count = 0;

  if (t->connection != NULL) {
    g_object_unref(t->connection);
    t->connection = NULL;
  }
  if (t->loop != NULL) {
    g_main_loop_unref(t->loop);
    t->loop = NULL;
  }
  if (t->context != NULL) {
    g_main_context_unref(t->context);
    t->context = NULL;
  }

  if (t->wake_read >= 0) {
    close(t->wake_read);
  }
  if (t->wake_write >= 0) {
    close(t->wake_write);
  }

  g_mutex_clear(&t->mutex);
  g_free(t);
}

int tray_event_fd(const tray_t *t) {
  return t != NULL ? t->wake_read : -1;
}

void tray_drain_events(tray_t *t) {
  if (t == NULL || t->wake_read < 0) {
    return;
  }
  char buffer[64];
  while (read(t->wake_read, buffer, sizeof(buffer)) > 0) {
  }
}

int tray_count(tray_t *t) {
  if (t == NULL) {
    return 0;
  }
  g_mutex_lock(&t->mutex);
  int count = 0;
  for (int i = 0; i < t->count; i++) {
    if (item_visible(&t->items[i])) {
      count++;
    }
  }
  g_mutex_unlock(&t->mutex);
  return count;
}

int tray_snapshot(tray_t *t, cairo_surface_t **out, int max) {
  if (t == NULL || out == NULL || max <= 0) {
    return 0;
  }

  g_mutex_lock(&t->mutex);
  int count = 0;
  for (int i = 0; i < t->count && count < max; i++) {
    if (!item_visible(&t->items[i])) {
      continue;
    }
    out[count++] = t->items[i].icon != NULL
                       ? cairo_surface_reference(t->items[i].icon)
                       : NULL;
  }
  g_mutex_unlock(&t->mutex);
  return count;
}

typedef struct tray_activation {
  GDBusConnection *connection;
  char *service;
  char *path;
  bool secondary;
} tray_activation_t;

static gpointer activate_thread(gpointer data) {
  tray_activation_t *activation = data;
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(
      activation->connection, activation->service, activation->path,
      SNI_INTERFACE,
      activation->secondary ? "SecondaryActivate" : "Activate",
      g_variant_new("(ii)", 0, 0), NULL, G_DBUS_CALL_FLAGS_NONE, 1500, NULL,
      &error);
  if (reply == NULL) {
    tlog("debar tray: activate %s%s failed: %s", activation->service,
         activation->path,
         error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
  } else {
    g_variant_unref(reply);
  }

  g_object_unref(activation->connection);
  g_free(activation->service);
  g_free(activation->path);
  g_free(activation);
  return NULL;
}

void tray_activate(tray_t *t, int index, bool secondary) {
  if (t == NULL || index < 0) {
    return;
  }

  tray_activation_t *activation = g_new0(tray_activation_t, 1);
  g_mutex_lock(&t->mutex);
  /* index counts visible items only (hidden applets are skipped) */
  const int real = visible_to_real_locked(t, index);
  if (real < 0 || t->connection == NULL) {
    g_mutex_unlock(&t->mutex);
    g_free(activation);
    return;
  }
  activation->connection = g_object_ref(t->connection);
  activation->service = g_strdup(t->items[real].service);
  activation->path = g_strdup(t->items[real].path);
  activation->secondary = secondary;
  g_mutex_unlock(&t->mutex);

  tlog("debar tray: activate %d secondary=%d -> %s%s", index,
       secondary ? 1 : 0, activation->service, activation->path);

  GThread *thread = g_thread_new("debar-tray-activate", activate_thread,
                                 activation);
  g_thread_unref(thread);
}
