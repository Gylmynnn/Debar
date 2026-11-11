#include "wayland.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "ipc.h"
#include "jobs.h"
#include "netmgr.h"
#include "sysinfo.h"

static modules_data_t wayland_data; /* live data, updated every second */

/* Copies the radio state known by the network managers into the bar data so
 * the Wi-Fi / Bluetooth icons can dim. Returns true when something changed. */
static bool sync_net_icons(void) {
  const int wifi = netmgr_wifi_icon_state();
  const int bt = netmgr_bt_icon_state();
  const bool changed = wifi != wayland_data.net_wifi || bt != wayland_data.net_bt;
  wayland_data.net_wifi = wifi;
  wayland_data.net_bt = bt;
  return changed;
}

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
#define EVDEV_KEY_BACKSPACE 14
#define EVDEV_KEY_ENTER 28
#define EVDEV_KEY_S 31
#define EVDEV_KEY_M 50
#define EVDEV_KEY_KPENTER 96
#define EVDEV_KEY_UP 103
#define EVDEV_KEY_LEFT 105
#define EVDEV_KEY_RIGHT 106
#define EVDEV_KEY_DOWN 108
#define EVDEV_BTN_LEFT 0x110
#define EVDEV_BTN_MIDDLE 0x112

/* a mouse wheel sends ~10 axis units per notch */
#define WHEEL_UNITS_PER_NOTCH 10.0

/* gap between the bar and a popup, in px */
#define CAL_BAR_GAP 6

/* An early main-loop wakeup (0 = none): wheel actions use it to re-read the
 * volume, and a freshly opened CPU popup uses it to get its first real
 * sample after a fraction of a second instead of a full tick. */
static long g_refresh_at_ms = 0;

/* Volume / brightness popup: every level change is a process spawn, so a
 * slider drag sends at most one command per LEVEL_DRAG_SEND_MS; the newest
 * value that had to wait is sent when the button is released. */
#define LEVEL_DRAG_SEND_MS 60
#define LEVEL_RECONCILE_MS 120
static long g_level_sent_ms = 0;
static int g_level_pending = -1;

/* Losing the keyboard focus dismisses a popup (see keyboard_leave), but a
 * compositor can also move the focus while the user is merely on the way to
 * the popup: some give an on-demand layer the focus as soon as it appears and
 * take it back when the pointer crosses the window under the gap between bar
 * and popup (focus-follows-mouse), which made the popup vanish before it could
 * be used. Focus loss is therefore ignored while the pointer has not reached
 * the popup yet (for a short while only, so a click elsewhere still closes it),
 * and during / right after a slider drag, which may end outside the popup. */
#define POPUP_REACH_GRACE_MS 2000
#define DRAG_END_GRACE_MS 500
static long g_popup_opened_ms = 0;
static bool g_popup_pointer_seen = false;
static long g_drag_end_ms = 0;

static long mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#define SYSINFO_FIRST_SAMPLE_MS 300

static bool popup_is_net(popup_kind_t kind) {
  return kind == POPUP_WIFI || kind == POPUP_BLUETOOTH;
}

static bool popup_is_sys(popup_kind_t kind) {
  return kind == POPUP_CPU || kind == POPUP_MEM;
}

static bool popup_is_level(popup_kind_t kind) {
  return kind == POPUP_VOLUME || kind == POPUP_BRIGHTNESS;
}

/* "42%" -> 42; an empty label (no audio server / no backlight) -> -1. */
static int label_percent(const char *label) {
  return label[0] != '\0' ? atoi(label) : -1;
}

/* At most this many released buffers are kept around for reuse. */
#define IDLE_BUFFERS_MAX 2

/* Returns a released buffer of the given size from the list, or creates one
 * (linked at the head). Reusing buffers avoids a memfd + mmap + pool +
 * wl_buffer round trip per frame. */
static bar_buffer_t *acquire_buffer(struct wl_shm *shm, bar_buffer_t **head,
                                    int width, int height) {
  for (bar_buffer_t *b = *head; b != NULL; b = b->next) {
    if (!b->busy && b->width == width && b->height == height) {
      return b;
    }
  }
  bar_buffer_t *created = render_buffer_create(shm, width, height);
  if (created != NULL) {
    created->next = *head;
    *head = created;
  }
  return created;
}

/* Frees released buffers of another size and any surplus idle ones. */
static void reap_idle_buffers(bar_buffer_t **head, int width, int height,
                              bar_buffer_t **active) {
  int idle = 0;
  bar_buffer_t **link = head;
  while (*link != NULL) {
    bar_buffer_t *b = *link;
    const bool stale = b->width != width || b->height != height;
    if (!b->busy && (stale || ++idle > IDLE_BUFFERS_MAX)) {
      *link = b->next;
      if (active != NULL && *active == b) {
        *active = NULL;
      }
      render_buffer_destroy(b);
    } else {
      link = &b->next;
    }
  }
}

static void popup_redraw(wayland_state_t *state) {
  if (state->popup_surface == NULL || !state->popup_configured) {
    return;
  }

  bar_buffer_t *buffer = acquire_buffer(state->shm, &state->popup_buffers,
                                        state->popup_w, state->popup_h);
  if (buffer == NULL) {
    fprintf(stderr, "debar: failed to create popup buffer\n");
    return;
  }

  switch (state->popup_kind) {
  case POPUP_CALENDAR:
    render_calendar(buffer, state->config, &state->calendar);
    break;
  case POPUP_WIFI:
    render_wifi_popup(buffer, state->config, netmgr_wifi(), &state->netui);
    break;
  case POPUP_BLUETOOTH:
    render_bt_popup(buffer, state->config, netmgr_bt(), &state->netui);
    break;
  case POPUP_CPU:
    render_cpu_popup(buffer, state->config, sysinfo_cpu());
    break;
  case POPUP_MEM:
    render_mem_popup(buffer, state->config, sysinfo_mem());
    break;
  case POPUP_VOLUME:
    render_volume_popup(buffer, state->config,
                        label_percent(wayland_data.volume),
                        wayland_data.volume_muted, &state->levelui);
    break;
  case POPUP_BRIGHTNESS:
    render_brightness_popup(buffer, state->config,
                            label_percent(wayland_data.brightness),
                            &state->levelui);
    break;
  case POPUP_NONE:
    return;
  }

  buffer->busy = true;
  state->popup_active = buffer;

  wl_surface_attach(state->popup_surface, buffer->wl_buffer, 0, 0);
  wl_surface_damage(state->popup_surface, 0, 0, state->popup_w, state->popup_h);
  wl_surface_commit(state->popup_surface);
  wl_display_flush(state->display);
}

/* ---- Wi-Fi password entry --------------------------------------------- */

static void wipe(void *p, size_t n) {
  volatile unsigned char *v = p;
  while (n-- > 0) {
    *v++ = 0;
  }
}

static void xkb_release(wayland_state_t *state) {
  if (state->xkb_state != NULL) {
    xkb_state_unref(state->xkb_state);
    state->xkb_state = NULL;
  }
  if (state->xkb_keymap != NULL) {
    xkb_keymap_unref(state->xkb_keymap);
    state->xkb_keymap = NULL;
  }
  if (state->xkb_context != NULL) {
    xkb_context_unref(state->xkb_context);
    state->xkb_context = NULL;
  }
}

/* Compiles the compositor's keymap, only while the password prompt is open
 * (a compiled keymap costs a few hundred KB we do not need the rest of the
 * time). */
static void xkb_prepare(wayland_state_t *state) {
  xkb_release(state);
  if (state->keymap_fd < 0 || state->keymap_size == 0) {
    return;
  }

  void *map = mmap(NULL, state->keymap_size, PROT_READ, MAP_PRIVATE,
                   state->keymap_fd, 0);
  if (map == MAP_FAILED) {
    return;
  }

  state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (state->xkb_context != NULL) {
    state->xkb_keymap = xkb_keymap_new_from_string(
        state->xkb_context, (const char *)map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
  }
  munmap(map, state->keymap_size);

  if (state->xkb_keymap != NULL) {
    state->xkb_state = xkb_state_new(state->xkb_keymap);
  }
  if (state->xkb_state != NULL) {
    xkb_state_update_mask(state->xkb_state, state->mods_depressed,
                          state->mods_latched, state->mods_locked, 0, 0,
                          state->mods_group);
  } else {
    xkb_release(state);
  }
}

static void password_enter(wayland_state_t *state, const char *ssid) {
  netpopup_ui_t *ui = &state->netui;
  ui->password_mode = true;
  snprintf(ui->password_ssid, sizeof(ui->password_ssid), "%s", ssid);
  ui->password_len = 0;
  wipe(state->password, sizeof(state->password));
  xkb_prepare(state);
}

static void password_leave(wayland_state_t *state, bool cancelled) {
  netpopup_ui_t *ui = &state->netui;
  ui->password_mode = false;
  ui->password_len = 0;
  wipe(state->password, sizeof(state->password));
  xkb_release(state);
  if (cancelled) {
    netmgr_wifi_cancel_password();
  }
}

static void level_end_drag(wayland_state_t *state);

static void popup_close(wayland_state_t *state) {
  if (state->popup_surface == NULL) {
    return;
  }

  level_end_drag(state); /* sends the last throttled value, releases the hold */

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
  state->popup_kind = POPUP_NONE;
  sysinfo_close(); /* stops the /proc sampling and frees its tables */
  if (state->netui.password_mode) {
    password_leave(state, true);
  }
  memset(&state->netui, 0, sizeof(state->netui));
  memset(&state->levelui, 0, sizeof(state->levelui));

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
  dlog("debar debug: popup closed: compositor closed its layer surface");
  popup_close((wayland_state_t *)data);
}

static const struct zwlr_layer_surface_v1_listener popup_layer_listener = {
    .configure = popup_layer_configure,
    .closed = popup_layer_closed,
};

/* Opens a popup just below (or above) the bar. The calendar is left-aligned
 * with the clock pill; the other popups are centered on the icon or pill that
 * opened them (anchor_x is its x inside the bar). */
static void popup_open(wayland_state_t *state, popup_kind_t kind,
                       double anchor_x) {
  if (state->popup_surface != NULL || !state->configured ||
      kind == POPUP_NONE) {
    return;
  }

  const bar_config_t *config = state->config;
  int popup_w = NETPOPUP_WIDTH;
  int popup_h = NETPOPUP_HEIGHT;
  switch (kind) {
  case POPUP_CALENDAR:
    popup_w = CAL_WIDTH;
    popup_h = CAL_HEIGHT;
    break;
  case POPUP_CPU:
    popup_w = SYSPOPUP_WIDTH;
    popup_h = render_cpu_popup_height(sysinfo_cpu_count());
    break;
  case POPUP_MEM:
    popup_w = SYSPOPUP_WIDTH;
    popup_h = render_mem_popup_height();
    break;
  case POPUP_VOLUME:
  case POPUP_BRIGHTNESS:
    popup_w = LEVELPOPUP_WIDTH;
    popup_h = render_level_popup_height(kind == POPUP_VOLUME);
    break;
  case POPUP_WIFI:
  case POPUP_BLUETOOTH:
  case POPUP_NONE:
    break;
  }

  int left = 0;
  if (kind == POPUP_CALENDAR) {
    calendar_goto_today(&state->calendar);
    double clock_x = 0.0;
    double clock_w = 0.0;
    render_clock_span(config, &wayland_data, &clock_x, &clock_w);
    left = config->margin.left + (int)clock_x;
  } else {
    left = config->margin.left + (int)(anchor_x - popup_w / 2.0);
  }

  const int screen_w =
      state->width + config->margin.left + config->margin.right;
  if (left + popup_w > screen_w - config->margin.right) {
    left = screen_w - config->margin.right - popup_w;
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
    fprintf(stderr, "debar: cannot create popup surface\n");
    return;
  }

  state->popup_layer = zwlr_layer_shell_v1_get_layer_surface(
      state->layer_shell, state->popup_surface, NULL,
      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "debar-popup");
  if (state->popup_layer == NULL) {
    fprintf(stderr, "debar: cannot create popup layer surface\n");
    wl_surface_destroy(state->popup_surface);
    state->popup_surface = NULL;
    return;
  }

  state->popup_kind = kind;
  state->popup_w = popup_w;
  state->popup_h = popup_h;
  memset(&state->netui, 0, sizeof(state->netui));
  memset(&state->levelui, 0, sizeof(state->levelui));
  g_level_pending = -1;
  g_popup_opened_ms = mono_ms();
  g_popup_pointer_seen = false;
  g_drag_end_ms = 0;
  if (kind == POPUP_WIFI) {
    netmgr_wifi_refresh();
  } else if (kind == POPUP_BLUETOOTH) {
    netmgr_bt_refresh();
  } else if (kind == POPUP_CPU) {
    /* the percentages need two samples: take the baseline now and the first
     * real sample shortly after, instead of showing zeros for a whole tick */
    sysinfo_open(SYSINFO_CPU);
    g_refresh_at_ms = mono_ms() + SYSINFO_FIRST_SAMPLE_MS;
  } else if (kind == POPUP_MEM) {
    sysinfo_open(SYSINFO_MEM);
  }

  zwlr_layer_surface_v1_set_size(state->popup_layer, popup_w, popup_h);
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

/* Clicking the icon of the popup that is already open closes it; clicking
 * another one switches. */
static void popup_toggle(wayland_state_t *state, popup_kind_t kind,
                         double anchor_x) {
  const bool same =
      state->popup_surface != NULL && state->popup_kind == kind;
  popup_close(state);
  if (!same) {
    popup_open(state, kind, anchor_x);
  }
}

/* ---- Wi-Fi / Bluetooth popup input ------------------------------------- */

static int net_row_count(const wayland_state_t *state) {
  if (state->popup_kind == POPUP_WIFI) {
    return netmgr_wifi()->count;
  }
  if (state->popup_kind == POPUP_BLUETOOTH) {
    return netmgr_bt()->count;
  }
  return 0;
}

static void net_clamp_scroll(wayland_state_t *state) {
  int max = net_row_count(state) - NETPOPUP_ROWS;
  if (max < 0) {
    max = 0;
  }
  if (state->netui.scroll > max) {
    state->netui.scroll = max;
  }
  if (state->netui.scroll < 0) {
    state->netui.scroll = 0;
  }
}

/* A job finished or an action changed the state: follow it on the bar icons
 * and in the open popup (including the Wi-Fi password prompt). */
static void net_state_changed(wayland_state_t *state) {
  if (sync_net_icons()) {
    wayland_redraw(state);
  }
  if (state->popup_surface == NULL || !popup_is_net(state->popup_kind)) {
    return;
  }

  if (state->popup_kind == POPUP_WIFI) {
    const netmgr_wifi_t *wifi = netmgr_wifi();
    if (wifi->need_password && !state->netui.password_mode) {
      password_enter(state, wifi->password_ssid);
    } else if (!wifi->need_password && state->netui.password_mode) {
      password_leave(state, false);
    }
  }
  net_clamp_scroll(state);
  popup_redraw(state);
}

static void net_handle_key(wayland_state_t *state, uint32_t key) {
  netpopup_ui_t *ui = &state->netui;

  if (key == EVDEV_KEY_ESC) {
    if (ui->password_mode) {
      password_leave(state, true); /* back to the list */
      popup_redraw(state);
    } else {
      popup_close(state);
    }
    return;
  }
  if (!ui->password_mode) {
    return;
  }

  if (key == EVDEV_KEY_ENTER || key == EVDEV_KEY_KPENTER) {
    if (state->password[0] != '\0') {
      netmgr_wifi_connect(ui->password_ssid, state->password);
      wipe(state->password, sizeof(state->password));
      ui->password_len = 0;
      net_state_changed(state);
    }
    return;
  }

  if (key == EVDEV_KEY_BACKSPACE) {
    size_t len = strlen(state->password);
    if (len > 0) {
      do { /* drop one UTF-8 character: the tail bytes, then its lead byte */
        len--;
      } while (len > 0 && ((unsigned char)state->password[len] & 0xC0) == 0x80);
      state->password[len] = '\0';
      ui->password_len--;
      popup_redraw(state);
    }
    return;
  }

  if (state->xkb_state == NULL) {
    return;
  }
  char text[16];
  const int n = xkb_state_key_get_utf8(state->xkb_state, key + 8, text,
                                       (size_t)sizeof(text));
  if (n <= 0 || (unsigned char)text[0] < 0x20 || text[0] == 0x7f) {
    return;
  }
  const size_t len = strlen(state->password);
  if (len + (size_t)n < sizeof(state->password)) {
    memcpy(state->password + len, text, (size_t)n);
    state->password[len + (size_t)n] = '\0';
    ui->password_len++;
    popup_redraw(state);
  }
}

static void net_click(wayland_state_t *state) {
  netpopup_ui_t *ui = &state->netui;
  int row = -1;
  const netpopup_hit_kind_t hit = render_netpopup_hit(
      state->pointer_x, state->pointer_y, ui->password_mode, &row);

  if (hit == NETPOPUP_HIT_TOGGLE) {
    if (state->popup_kind == POPUP_WIFI && netmgr_wifi()->available) {
      netmgr_wifi_set_enabled(!netmgr_wifi()->enabled);
    } else if (state->popup_kind == POPUP_BLUETOOTH && netmgr_bt()->available) {
      netmgr_bt_set_powered(!netmgr_bt()->powered);
    }
  } else if (hit == NETPOPUP_HIT_ROW && !ui->password_mode) {
    const int index = ui->scroll + row;
    if (state->popup_kind == POPUP_WIFI) {
      const netmgr_wifi_t *wifi = netmgr_wifi();
      if (index >= 0 && index < wifi->count) {
        if (wifi->nets[index].active) {
          netmgr_wifi_disconnect();
        } else {
          netmgr_wifi_connect(wifi->nets[index].ssid, NULL);
        }
      }
    } else if (state->popup_kind == POPUP_BLUETOOTH) {
      netmgr_bt_toggle_device(index);
    }
  }

  (void)netmgr_take_dirty();
  net_state_changed(state);
}

/* Mouse wheel scrolls the list (accumulated: touchpads send fractions). */
static void net_scroll(wayland_state_t *state, double value) {
  static double acc = 0.0;
  acc += value;
  int rows = 0;
  while (acc >= WHEEL_UNITS_PER_NOTCH) {
    rows++;
    acc -= WHEEL_UNITS_PER_NOTCH;
  }
  while (acc <= -WHEEL_UNITS_PER_NOTCH) {
    rows--;
    acc += WHEEL_UNITS_PER_NOTCH;
  }
  if (rows == 0) {
    return;
  }
  const int before = state->netui.scroll;
  state->netui.scroll += rows;
  net_clamp_scroll(state);
  if (state->netui.scroll != before) {
    popup_redraw(state);
  }
}

static void net_hover(wayland_state_t *state) {
  netpopup_ui_t *ui = &state->netui;
  int row = -1;
  const netpopup_hit_kind_t hit = render_netpopup_hit(
      state->pointer_x, state->pointer_y, ui->password_mode, &row);
  const int slot = hit == NETPOPUP_HIT_ROW ? row : -1;
  if (hit != ui->hover_kind || slot != (ui->hover_kind == NETPOPUP_HIT_ROW ? ui->hover_row : -1)) {
    ui->hover_kind = hit;
    ui->hover_row = slot;
    popup_redraw(state);
  }
}

/* ---- Volume / brightness popup input ------------------------------------ */

static int level_percent(const wayland_state_t *state) {
  return label_percent(state->popup_kind == POPUP_VOLUME
                           ? wayland_data.volume
                           : wayland_data.brightness);
}

/* Sends the new level to wpctl/pactl or brightnessctl (the label follows). */
static void level_send(wayland_state_t *state, int percent) {
  if (state->popup_kind == POPUP_VOLUME) {
    modules_volume_set(&wayland_data, percent);
  } else {
    modules_brightness_set(&wayland_data, percent);
  }
  g_level_sent_ms = mono_ms();
  g_level_pending = -1;
}

/* Moves the shown label without running a command (throttled drag steps). */
static void level_set_label(const wayland_state_t *state, int percent) {
  if (state->popup_kind == POPUP_VOLUME) {
    snprintf(wayland_data.volume, sizeof(wayland_data.volume), "%d%%", percent);
  } else {
    snprintf(wayland_data.brightness, sizeof(wayland_data.brightness), "%d%%",
             percent < 1 ? 1 : percent);
  }
}

/* Moves the level to percent. Clicks and key presses pass throttle = false;
 * a drag passes true and keeps the command rate down. The bar and the popup
 * are only repainted when the value really changed. */
static void level_set(wayland_state_t *state, int percent, bool throttle) {
  const int current = level_percent(state);
  if (current < 0) {
    return; /* no audio server / no backlight */
  }
  if (percent > 100) {
    percent = 100;
  }
  if (percent < (state->popup_kind == POPUP_BRIGHTNESS ? 1 : 0)) {
    percent = state->popup_kind == POPUP_BRIGHTNESS ? 1 : 0;
  }
  if (percent == current) {
    return;
  }

  if (throttle && mono_ms() - g_level_sent_ms < LEVEL_DRAG_SEND_MS) {
    level_set_label(state, percent);
    g_level_pending = percent;
  } else {
    level_send(state, percent);
  }
  wayland_redraw(state);
  popup_redraw(state);
}

static void level_nudge(wayland_state_t *state, int delta) {
  const int current = level_percent(state);
  if (current < 0) {
    return;
  }
  level_set(state, current + delta, false);
  g_refresh_at_ms = mono_ms() + LEVEL_RECONCILE_MS;
}

static void level_toggle_mute(wayland_state_t *state) {
  modules_volume_toggle_mute(&wayland_data);
  wayland_redraw(state);
  popup_redraw(state);
  g_refresh_at_ms = mono_ms() + LEVEL_RECONCILE_MS;
}

static void level_end_drag(wayland_state_t *state) {
  if (!state->levelui.dragging) {
    return;
  }
  state->levelui.dragging = false;
  g_drag_end_ms = mono_ms();
  if (g_level_pending >= 0 && popup_is_level(state->popup_kind)) {
    level_send(state, g_level_pending);
  }
  g_level_pending = -1;
  modules_hold_levels(false);
  g_refresh_at_ms = mono_ms() + LEVEL_RECONCILE_MS;
  popup_redraw(state);
}

static void level_click(wayland_state_t *state) {
  const bool is_volume = state->popup_kind == POPUP_VOLUME;
  int value = 0;
  const levelpopup_hit_kind_t hit = render_levelpopup_hit(
      is_volume, state->pointer_x, state->pointer_y, &value, NULL);

  if (hit == LEVELPOPUP_HIT_SLIDER) {
    /* grab the slider: until the button is released the pointer position
     * drives the value, and the once-per-second re-read stays out of it */
    state->levelui.dragging = true;
    modules_hold_levels(true);
    level_set(state, value, false);
    popup_redraw(state);
  } else if (hit == LEVELPOPUP_HIT_PRESET) {
    level_set(state, value, false);
    g_refresh_at_ms = mono_ms() + LEVEL_RECONCILE_MS;
  } else if (hit == LEVELPOPUP_HIT_MUTE && is_volume) {
    level_toggle_mute(state);
  }
}

static void level_motion(wayland_state_t *state) {
  levelpopup_ui_t *ui = &state->levelui;
  if (ui->dragging) {
    /* the pointer may leave the slider's box while the button is held */
    level_set(state, render_levelpopup_slider_value(state->pointer_x), true);
    return;
  }

  int index = -1;
  const levelpopup_hit_kind_t hit =
      render_levelpopup_hit(state->popup_kind == POPUP_VOLUME, state->pointer_x,
                            state->pointer_y, NULL, &index);
  const int preset = hit == LEVELPOPUP_HIT_PRESET ? index : -1;
  if (hit != ui->hover_kind ||
      (hit == LEVELPOPUP_HIT_PRESET && preset != ui->hover_preset)) {
    ui->hover_kind = hit;
    ui->hover_preset = preset;
    popup_redraw(state);
  }
}

/* Mouse wheel over the popup: whole notches move the level by 5 %. */
static void level_scroll(wayland_state_t *state, double value) {
  static double acc = 0.0;
  acc += value;
  int notches = 0;
  while (acc >= WHEEL_UNITS_PER_NOTCH) {
    notches++;
    acc -= WHEEL_UNITS_PER_NOTCH;
  }
  while (acc <= -WHEEL_UNITS_PER_NOTCH) {
    notches--;
    acc += WHEEL_UNITS_PER_NOTCH;
  }
  if (notches == 0) {
    return;
  }
  /* wheel down (positive axis) lowers the value, like on the bar pill */
  level_nudge(state, -notches * LEVELPOPUP_STEP);
}

static void level_handle_key(wayland_state_t *state, uint32_t key) {
  switch (key) {
  case EVDEV_KEY_ESC:
  case EVDEV_KEY_Q:
    popup_close(state);
    break;
  case EVDEV_KEY_LEFT:
  case EVDEV_KEY_DOWN:
    level_nudge(state, -LEVELPOPUP_STEP);
    break;
  case EVDEV_KEY_RIGHT:
  case EVDEV_KEY_UP:
    level_nudge(state, LEVELPOPUP_STEP);
    break;
  case EVDEV_KEY_M:
    if (state->popup_kind == POPUP_VOLUME) {
      level_toggle_mute(state);
    }
    break;
  default:
    break;
  }
}

static void popup_handle_key(wayland_state_t *state, uint32_t key) {
  if (popup_is_level(state->popup_kind)) {
    level_handle_key(state, key);
    return;
  }
  if (popup_is_net(state->popup_kind)) {
    net_handle_key(state, key);
    return;
  }
  if (popup_is_sys(state->popup_kind)) {
    if (key == EVDEV_KEY_ESC || key == EVDEV_KEY_Q) {
      popup_close(state);
    }
    return;
  }

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
  if (popup_is_sys(state->popup_kind)) {
    return; /* nothing to scroll */
  }
  if (popup_is_level(state->popup_kind)) {
    level_scroll(state, value);
    return;
  }
  if (popup_is_net(state->popup_kind)) {
    net_scroll(state, value);
    return;
  }

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
  /* A click inside a popup: the calendar only takes keyboard focus, the
   * Wi-Fi / Bluetooth managers act on what was clicked. */
  if (state->popup_surface != NULL &&
      state->pointer_surface == state->popup_surface) {
    if (popup_is_net(state->popup_kind) && button == EVDEV_BTN_LEFT) {
      net_click(state);
    } else if (popup_is_level(state->popup_kind) && button == EVDEV_BTN_LEFT) {
      level_click(state);
    }
    return;
  }

  /* Left click on the clock pill toggles the calendar. */
  double clock_x = 0.0;
  double clock_w = 0.0;
  if (button == EVDEV_BTN_LEFT &&
      render_clock_span(state->config, &wayland_data, &clock_x, &clock_w) &&
      state->pointer_x >= clock_x && state->pointer_x <= clock_x + clock_w) {
    popup_toggle(state, POPUP_CALENDAR, 0.0);
    return;
  }

  /* Left click on the Wi-Fi / Bluetooth icon toggles its manager. */
  double icon_x = 0.0;
  const render_status_icon_t icon =
      button == EVDEV_BTN_LEFT
          ? render_status_icon_hit(state->config, &wayland_data, state->tray,
                                   (double)state->width, state->pointer_x,
                                   state->pointer_y, &icon_x)
          : RENDER_ICON_NONE;
  if (icon != RENDER_ICON_NONE) {
    popup_toggle(state,
                 icon == RENDER_ICON_WIFI ? POPUP_WIFI : POPUP_BLUETOOTH,
                 icon_x);
    return;
  }

  /* Left click on the CPU or RAM pill toggles its detail popup. */
  double pill_x = 0.0;
  const render_sys_pill_t pill =
      button == EVDEV_BTN_LEFT
          ? render_sys_pill_hit(state->config, &wayland_data, state->tray,
                                (double)state->width, state->pointer_x,
                                state->pointer_y, &pill_x)
          : RENDER_SYS_NONE;
  if (pill != RENDER_SYS_NONE) {
    popup_toggle(state, pill == RENDER_SYS_CPU ? POPUP_CPU : POPUP_MEM,
                 pill_x);
    return;
  }

  /* Left click on the volume or brightness half of the pill toggles its
   * popup. (The wheel over the pill still changes the value directly.) */
  double level_x = 0.0;
  const render_scroll_target_t level =
      button == EVDEV_BTN_LEFT
          ? render_level_pill_hit(state->config, &wayland_data, state->tray,
                                  (double)state->width, state->pointer_x,
                                  state->pointer_y, &level_x)
          : RENDER_SCROLL_NONE;
  if (level != RENDER_SCROLL_NONE) {
    popup_toggle(state,
                 level == RENDER_SCROLL_VOLUME ? POPUP_VOLUME
                                               : POPUP_BRIGHTNESS,
                 level_x);
    return;
  }

  /* Any other click on the bar dismisses an open popup. */
  if (state->popup_surface != NULL) {
    dlog("debar debug: popup closed: click at (%.1f, %.1f) on the bar "
         "(pointer surface is %s)",
         state->pointer_x, state->pointer_y,
         state->pointer_surface == state->surface ? "the bar" : "unknown");
  }
  popup_close(state);

  /* Tray icons: left activates the item, middle asks for its secondary
   * action (usually "open menu"). */
  if (button == EVDEV_BTN_LEFT || button == EVDEV_BTN_MIDDLE) {
    int tray_index = -1;
    if (render_tray_hit(state->config, state->tray, (double)state->width,
                        state->pointer_x, state->pointer_y, &tray_index)) {
      tray_activate(state->tray, tray_index, button == EVDEV_BTN_MIDDLE);
      dlog("debar debug: tray click index=%d secondary=%d", tray_index,
           button == EVDEV_BTN_MIDDLE ? 1 : 0);
      return;
    }
  }

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
#define WHEEL_STEP_PERCENT 5
#define WHEEL_MAX_NOTCHES 5

/* The label moves instantly; give the external tool a moment, then reconcile
 * with the authoritative value from the next poll. */
#define ACTION_REFRESH_DELAY_MS 120
static double g_wheel_acc = 0.0;

static void pointer_handle_axis(wayland_state_t *state, double value) {
  /* The wheel never switches workspaces - that belongs to the compositor's
   * keybinds. It only acts on the volume/brightness pill. */
  const render_scroll_target_t target = render_scroll_hit(
      state->config, &wayland_data, state->tray, (double)state->width,
      state->pointer_x, state->pointer_y);
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
  if (state->popup_surface != NULL && popup_is_level(state->popup_kind)) {
    popup_redraw(state);
  }
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
  if (surface != NULL && surface == state->popup_surface) {
    g_popup_pointer_seen = true;
  }
  state->pointer_x = wl_fixed_to_double(surface_x);
  state->pointer_y = wl_fixed_to_double(surface_y);
  dlog("debar debug: pointer enter %s (%.1f, %.1f)",
       surface == state->popup_surface && surface != NULL ? "popup" : "bar",
       state->pointer_x, state->pointer_y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
  (void)serial;
  (void)pointer;
  wayland_state_t *state = data;
  dlog("debar debug: pointer leave %s",
       surface == state->popup_surface && surface != NULL ? "popup" : "bar");
  if (state->pointer_surface == surface) {
    state->pointer_surface = NULL;
  }
  if (surface != NULL && surface == state->popup_surface &&
      popup_is_net(state->popup_kind) &&
      state->netui.hover_kind != NETPOPUP_HIT_NONE) {
    state->netui.hover_kind = NETPOPUP_HIT_NONE; /* clear the hover highlight */
    popup_redraw(state);
  }
  if (surface != NULL && surface == state->popup_surface &&
      popup_is_level(state->popup_kind) && !state->levelui.dragging &&
      state->levelui.hover_kind != LEVELPOPUP_HIT_NONE) {
    state->levelui.hover_kind = LEVELPOPUP_HIT_NONE;
    popup_redraw(state);
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
  if (state->popup_surface != NULL &&
      state->pointer_surface == state->popup_surface &&
      popup_is_net(state->popup_kind)) {
    net_hover(state);
  }
  /* A slider drag keeps receiving motion (implicit grab) even when the
   * pointer leaves the popup, so it must not depend on the pointer surface. */
  if (state->popup_surface != NULL && popup_is_level(state->popup_kind) &&
      (state->levelui.dragging ||
       state->pointer_surface == state->popup_surface)) {
    level_motion(state);
  }
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
  } else if (button == EVDEV_BTN_LEFT) {
    level_end_drag((wayland_state_t *)data); /* no-op unless a slider drag */
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
  (void)keyboard;
  wayland_state_t *state = data;
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(fd);
    return;
  }
  /* Keep the fd (it costs nothing); the keymap is compiled on demand, while
   * the Wi-Fi password prompt is open. */
  if (state->keymap_fd >= 0) {
    close(state->keymap_fd);
  }
  state->keymap_fd = fd;
  state->keymap_size = size;
  if (state->netui.password_mode) {
    xkb_prepare(state);
  }
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
  (void)keyboard;
  (void)serial;
  (void)keys;
  wayland_state_t *state = data;
  state->keyboard_surface = surface;
  dlog("debar debug: keyboard focus -> %s",
       surface == state->popup_surface && surface != NULL ? "popup" : "bar");
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface) {
  (void)keyboard;
  (void)serial;
  wayland_state_t *state = data;
  dlog("debar debug: keyboard leave %s (pointer is over %s, dragging=%d)",
       surface == state->popup_surface && surface != NULL ? "popup" : "bar",
       state->pointer_surface == NULL ? "nothing"
       : state->pointer_surface == state->popup_surface ? "the popup" : "the bar",
       state->levelui.dragging ? 1 : 0);
  if (state->keyboard_surface == surface) {
    state->keyboard_surface = NULL;
  }
  if (state->popup_surface != NULL && surface == state->popup_surface) {
    const long now = mono_ms();
    const char *keep = NULL;
    if (state->levelui.dragging) {
      keep = "a slider is being dragged";
    } else if (g_drag_end_ms != 0 && now - g_drag_end_ms < DRAG_END_GRACE_MS) {
      keep = "a slider drag just ended";
    } else if (!g_popup_pointer_seen &&
               now - g_popup_opened_ms < POPUP_REACH_GRACE_MS) {
      keep = "the pointer has not reached the popup yet";
    }
    if (keep != NULL) {
      dlog("debar debug: keyboard focus left the popup, keeping it open: %s",
           keep);
      return;
    }
    /* focus moved to another window: dismiss the popup */
    dlog("debar debug: popup closed: keyboard focus moved to another surface");
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
  (void)keyboard;
  (void)serial;
  wayland_state_t *state = data;
  state->mods_depressed = mods_depressed;
  state->mods_latched = mods_latched;
  state->mods_locked = mods_locked;
  state->mods_group = group;
  if (state->xkb_state != NULL) {
    xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched,
                          mods_locked, 0, 0, group);
  }
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
  state->keymap_fd = -1;
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
  state->tray = tray_create();
  return true;
}

/* Inputs of the last frame that was actually drawn: a one-second tick whose
 * data did not change does not need a new frame. */
static modules_data_t g_last_drawn;
static bool g_have_last_drawn = false;

/* Renders one frame from the current live data. */
void wayland_redraw(wayland_state_t *state) {
  if (state == NULL || !state->configured || state->width <= 0) {
    return;
  }

  bar_buffer_t *buffer =
      acquire_buffer(state->shm, &state->buffers, state->width, state->height);
  if (buffer == NULL) {
    fprintf(stderr, "debar: failed to create Wayland buffer\n");
    return;
  }

  (void)sync_net_icons();
  render_bar(buffer, state->config, &wayland_data, state->tray);

  buffer->busy = true;
  state->active_buffer = buffer;
  memcpy(&g_last_drawn, &wayland_data, sizeof(g_last_drawn));
  g_have_last_drawn = true;

  wl_surface_attach(state->surface, buffer->wl_buffer, 0, 0);
  wl_surface_damage(state->surface, 0, 0, state->width, state->height);
  wl_surface_commit(state->surface);
  wl_display_flush(state->display);
}

static void reap_retired_buffers(wayland_state_t *state) {
  reap_idle_buffers(&state->buffers, state->width, state->height,
                    &state->active_buffer);
  reap_idle_buffers(&state->popup_buffers, state->popup_w, state->popup_h,
                    &state->popup_active);
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
  const int tray_fd =
      state->tray != NULL ? tray_event_fd(state->tray) : -1;

  struct pollfd fds[3 + JOBS_MAX]; /* display, ipc, tray + async jobs */
  int nfds = 0;
  const int display_index = nfds++;
  fds[display_index].fd = display_fd;
  fds[display_index].events = POLLIN;
  fds[display_index].revents = 0;

  int ipc_index = -1;
  if (ipc_fd >= 0) {
    ipc_index = nfds++;
    fds[ipc_index].fd = ipc_fd;
    fds[ipc_index].events = POLLIN;
    fds[ipc_index].revents = 0;
  }

  int tray_index = -1;
  if (tray_fd >= 0) {
    tray_index = nfds++;
    fds[tray_index].fd = tray_fd;
    fds[tray_index].events = POLLIN;
    fds[tray_index].revents = 0;
  }

  while (state->running) {
    wl_display_flush(state->display);
    fds[display_index].events = POLLIN;
    if (ipc_index >= 0) {
      fds[ipc_index].events = POLLIN;
    }
    if (tray_index >= 0) {
      fds[tray_index].events = POLLIN;
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

    /* Async Wi-Fi / Bluetooth commands: their pipes join the poll set, and a
     * command that overstays its deadline wakes the loop to be killed. */
    const int job_count = jobs_fill(&fds[nfds], JOBS_MAX);
    const int job_timeout = jobs_timeout_ms();
    const bool job_wakes_first = job_timeout >= 0 && job_timeout < timeout;
    if (job_wakes_first) {
      timeout = job_timeout;
    }
    const int poll_ret = poll(fds, (nfds_t)(nfds + job_count), timeout);

    if (poll_ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "debar: poll failed\n");
      state->running = false;
      return -1;
    }

    if (poll_ret > 0) {
      if ((fds[display_index].revents & POLLIN) != 0) {
        if (wl_display_dispatch(state->display) < 0 && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
          fprintf(stderr, "debar: Wayland event dispatch failed\n");
          state->running = false;
          return -1;
        }
      }
      if (ipc_index >= 0 && (fds[ipc_index].revents & POLLIN) != 0) {
        /* workspace events: update state and repaint immediately */
        ipc_handle_events(&wayland_data);
        wayland_redraw(state);
      }
      if (tray_index >= 0 && (fds[tray_index].revents & POLLIN) != 0) {
        /* tray items appeared/disappeared or changed their icon */
        tray_drain_events(state->tray);
        wayland_redraw(state);
      }
      if ((fds[display_index].revents & (POLLERR | POLLHUP)) != 0) {
        state->running = false;
        break;
      }
      if (ipc_index >= 0 &&
          (fds[ipc_index].revents & (POLLERR | POLLHUP)) != 0) {
        state->running = false;
        break;
      }
    }

    jobs_handle(&fds[nfds], job_count);
    if (netmgr_take_dirty()) {
      net_state_changed(state);
    }

    if (poll_ret == 0 && !job_wakes_first) {
      /* One-second tick (or an early refresh requested by a wheel action):
       * refresh live data and repaint. Workspaces are always re-queried so
       * occupancy ("has a program") stays accurate even when the event
       * stream is connected; the switch grace window keeps this poll from
       * reverting a just-clicked active workspace. */
      modules_update(&wayland_data);
      ipc_query_workspaces(&wayland_data);
      netmgr_tick(state->popup_surface != NULL && state->popup_kind == POPUP_WIFI,
                  state->popup_surface != NULL &&
                      state->popup_kind == POPUP_BLUETOOTH);
      (void)sync_net_icons();
      if (state->popup_surface != NULL && popup_is_sys(state->popup_kind)) {
        sysinfo_sample(); /* /proc is only walked while a CPU/RAM popup is up */
        popup_redraw(state);
      }
      if (state->popup_surface != NULL && popup_is_level(state->popup_kind) &&
          !state->levelui.dragging) {
        popup_redraw(state); /* follows volume keys, other mixers, ... */
      }
      if (!g_have_last_drawn ||
          memcmp(&g_last_drawn, &wayland_data, sizeof(wayland_data)) != 0) {
        wayland_redraw(state);
      }
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

  tray_destroy(state->tray);
  state->tray = NULL;
  ipc_finish();
  popup_close(state);
  xkb_release(state);
  if (state->keymap_fd >= 0) {
    close(state->keymap_fd);
    state->keymap_fd = -1;
  }
  wipe(state->password, sizeof(state->password));
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
