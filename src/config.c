#include "config.h"

#include <stddef.h>

void config_load_defaults(bar_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = (bar_config_t) {
        .height = 53,
        .margin_top = 6,
        .margin_side = 5,
        .exclusive = 59,          /* 53 + 6 below; compositor adds margin_top */
        .radius = 8,
        .pill_height = 35,
        .padding = 24,
        .gap = 24,
        .font_family = "JetBrainsMono Nerd Font",
        .font_size = 18.0,
        .background = "#141b1e",
        .foreground = "#dadada",
        .segment = "#232a2d",
    };
}
