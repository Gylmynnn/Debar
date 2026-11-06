#ifndef CBAR_CONFIG_H
#define CBAR_CONFIG_H

typedef struct bar_config {
    int height;
    int padding;
    const char *font;
    const char *background;
    const char *foreground;
} bar_config_t;

void config_load_defaults(bar_config_t *config);

#endif
