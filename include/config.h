#ifndef DEBAR_CONFIG_H
#define DEBAR_CONFIG_H

/* Which screen edge the bar is attached to. */
typedef enum bar_position {
  BAR_POSITION_TOP,
  BAR_POSITION_BOTTOM,
} bar_position_t;

/* A value per side, in px (used for padding and margin). */
typedef struct bar_sides {
  int top;
  int bottom;
  int left;
  int right;
} bar_sides_t;

typedef struct bar_config {
  bar_position_t position;
  bar_sides_t padding; /* bar edge -> pills; also decides the bar height */
  bar_sides_t margin;  /* gap between the bar and the screen edges / windows:
                        * the side the bar is attached to is the distance from
                        * the screen edge, the opposite side is the space left
                        * between the bar and the windows */

  int height;      /* bar height in px, derived: pill_height + padding top/bottom */
  int radius;      /* bar corner radius */
  int pill_height; /* height of every pill inside the bar */
  int gap;         /* gap between pills */
  const char *font_family;
  double font_size;       /* in pixels (absolute), not points */
  const char *background; /* bar background, supports alpha */
  const char *foreground; /* clock text */
  const char *segment;    /* pill background */
} bar_config_t;

/* Built-in defaults (what the bar looked like before the Lua config). */
void config_load_defaults(bar_config_t *config);

/* Recomputes values derived from other fields (the bar height). Call it after
 * changing padding or pill_height by hand. */
void config_finalize(bar_config_t *config);

/* Defaults, then ~/.config/debar/config.lua (or $XDG_CONFIG_HOME/debar/...)
 * when the file exists. A broken config never stops the bar: problems are
 * printed to stderr and the affected options keep their defaults. */
void config_load(bar_config_t *config);

#endif
