#ifndef DEBAR_CONFIG_H
#define DEBAR_CONFIG_H

typedef struct bar_config {
  int height;      /* bar height in px (the floating rounded rectangle) */
  int margin_top;  /* gap between screen top and bar */
  int margin_side; /* gap between screen left/right edge and bar */
  int exclusive; /* space reserved below the bar (compositor adds margin_top) */
  int radius;    /* bar corner radius */
  int pill_height; /* height of every pill inside the bar */
  int padding;     /* inner padding: bar edge -> first/last module */
  int gap;         /* gap between pills */
  const char *font_family;
  double font_size;       /* in pixels (absolute), not points */
  const char *background; /* bar background, supports alpha */
  const char *foreground; /* clock text */
  const char *segment;    /* pill background */
} bar_config_t;

void config_load_defaults(bar_config_t *config);

#endif
