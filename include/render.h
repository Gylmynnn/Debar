#ifndef DEBAR_RENDER_H
#define DEBAR_RENDER_H

#include "calendar.h"
#include "config.h"
#include "modules.h"
#include "netmgr.h"
#include "sysinfo.h"
#include "tray.h"
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
                const modules_data_t *data, tray_t *tray);
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

/* Status modules that consume the mouse wheel. The wheel never switches
 * workspaces - that belongs to the compositor's keybinds. */
typedef enum render_scroll_target {
  RENDER_SCROLL_NONE = 0,
  RENDER_SCROLL_VOLUME,
  RENDER_SCROLL_BRIGHTNESS,
} render_scroll_target_t;

/* Which status module (volume/brightness pill) sits under (px, py)? Uses
 * the same geometry as render_bar, so the wheel only acts on that pill. */
render_scroll_target_t render_scroll_hit(const bar_config_t *config,
                                         const modules_data_t *data,
                                         tray_t *tray, double bar_width,
                                         double px, double py);

/* Like render_scroll_hit, but for clicks: which half of the volume/brightness
 * pill sits under (px, py)? center_x (optional) receives the horizontal center
 * of that half, used to place its popup. */
render_scroll_target_t render_level_pill_hit(const bar_config_t *config,
                                             const modules_data_t *data,
                                             tray_t *tray, double bar_width,
                                             double px, double py,
                                             double *center_x);

/* Which tray icon sits under (px, py)? Returns false outside the tray pill. */
bool render_tray_hit(const bar_config_t *config, tray_t *tray,
                     double bar_width, double px, double py, int *index_out);

/* Status icons in the app strip (just before the volume pill) that open a
 * manager popup. */
typedef enum render_status_icon {
  RENDER_ICON_NONE = 0,
  RENDER_ICON_WIFI,
  RENDER_ICON_BLUETOOTH,
} render_status_icon_t;

/* Which status icon sits under (px, py)? center_x (optional) receives the
 * icon's horizontal center, used to place its popup. */
render_status_icon_t render_status_icon_hit(const bar_config_t *config,
                                            const modules_data_t *data,
                                            tray_t *tray, double bar_width,
                                            double px, double py,
                                            double *center_x);

/* The CPU and RAM pills open a detail popup. */
typedef enum render_sys_pill {
  RENDER_SYS_NONE = 0,
  RENDER_SYS_CPU,
  RENDER_SYS_MEM,
} render_sys_pill_t;

/* Which of the CPU / RAM pills sits under (px, py)? center_x (optional)
 * receives the pill's horizontal center, used to place its popup. */
render_sys_pill_t render_sys_pill_hit(const bar_config_t *config,
                                      const modules_data_t *data, tray_t *tray,
                                      double bar_width, double px, double py,
                                      double *center_x);

/* ---- Wi-Fi / Bluetooth manager popups ----------------------------------- */

#define NETPOPUP_WIDTH 320
#define NETPOPUP_HEIGHT 388
#define NETPOPUP_ROWS 6 /* visible list rows */

typedef enum netpopup_hit_kind {
  NETPOPUP_HIT_NONE = 0,
  NETPOPUP_HIT_TOGGLE, /* the on/off switch in the header */
  NETPOPUP_HIT_ROW,    /* a list row; row_out = visible slot 0..ROWS-1 */
  NETPOPUP_HIT_FIELD,  /* the password box */
} netpopup_hit_kind_t;

/* UI state shared between the event code and the renderer. The typed password
 * itself never leaves the event code: the renderer only needs its length. */
typedef struct netpopup_ui {
  netpopup_hit_kind_t hover_kind;
  int hover_row;
  int scroll; /* index of the first visible list row */
  bool password_mode;
  char password_ssid[64];
  int password_len; /* characters typed so far */
} netpopup_ui_t;

netpopup_hit_kind_t render_netpopup_hit(double px, double py,
                                        bool password_mode, int *row_out);

void render_wifi_popup(bar_buffer_t *buffer, const bar_config_t *config,
                       const netmgr_wifi_t *wifi, const netpopup_ui_t *ui);
void render_bt_popup(bar_buffer_t *buffer, const bar_config_t *config,
                     const netmgr_bt_t *bt, const netpopup_ui_t *ui);

/* ---- CPU / RAM popups ----------------------------------------------------- */

#define SYSPOPUP_WIDTH 320

/* Popup height in px. The RAM popup is fixed; the CPU popup grows with the
 * number of cores it lists. */
int render_cpu_popup_height(int cores);
int render_mem_popup_height(void);

void render_cpu_popup(bar_buffer_t *buffer, const bar_config_t *config,
                      const sysinfo_cpu_t *cpu);
void render_mem_popup(bar_buffer_t *buffer, const bar_config_t *config,
                      const sysinfo_mem_t *mem);

/* ---- Volume / brightness popups -------------------------------------------- */

#define LEVELPOPUP_WIDTH 320
#define LEVELPOPUP_STEP 5 /* percent per wheel notch / arrow key */

typedef enum levelpopup_hit_kind {
  LEVELPOPUP_HIT_NONE = 0,
  LEVELPOPUP_HIT_SLIDER, /* the slider; value_out = percent under the pointer */
  LEVELPOPUP_HIT_PRESET, /* a preset button; value_out = its percent */
  LEVELPOPUP_HIT_MUTE,   /* the mute row (volume popup only) */
} levelpopup_hit_kind_t;

/* UI state shared between the event code and the renderer. */
typedef struct levelpopup_ui {
  levelpopup_hit_kind_t hover_kind;
  int hover_preset; /* index of the hovered preset button */
  bool dragging;    /* slider is being dragged */
} levelpopup_ui_t;

/* Popup height in px (the volume popup has an extra mute row). */
int render_level_popup_height(bool is_volume);

/* What sits under (px, py) inside the popup? value_out / index_out (both
 * optional) receive the percent and the preset index where they apply. */
levelpopup_hit_kind_t render_levelpopup_hit(bool is_volume, double px,
                                            double py, int *value_out,
                                            int *index_out);

/* Percent (0..100) that a pointer at px means on the slider, clamped. Used
 * while dragging, when the pointer may leave the slider's box. */
int render_levelpopup_slider_value(double px);

/* percent < 0: the value is unavailable (no audio server / no backlight). */
void render_volume_popup(bar_buffer_t *buffer, const bar_config_t *config,
                         int percent, bool muted, const levelpopup_ui_t *ui);
void render_brightness_popup(bar_buffer_t *buffer, const bar_config_t *config,
                             int percent, const levelpopup_ui_t *ui);

/* Draws the floating calendar popup (CAL_WIDTH x CAL_HEIGHT buffer). */
void render_calendar(bar_buffer_t *buffer, const bar_config_t *config,
                     const calendar_t *cal);

#endif
