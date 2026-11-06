#include "config.h"

#include <stddef.h>

void config_load_defaults(bar_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = (bar_config_t) {
        .height = 32,
        .padding = 12,
        .font = "Inter 10",
        .background = "#1e1e2e",
        .foreground = "#cdd6f4",
    };
}
