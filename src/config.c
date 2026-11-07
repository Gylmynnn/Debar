#include "config.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONFIG_PADDING_MAX 1000

void config_finalize(bar_config_t *config) {
  if (config == NULL) {
    return;
  }
  config->height =
      config->pill_height + config->padding.top + config->padding.bottom;
}

void config_load_defaults(bar_config_t *config) {
  if (config == NULL) {
    return;
  }

  *config = (bar_config_t){
      .position = BAR_POSITION_TOP,
      /* 9 + 35 + 9 = 53 px tall, pills start 24 px from the left edge */
      .padding = {.top = 9, .bottom = 9, .left = 24, .right = 24},
      .margin = {.top = 6, .bottom = 6, .left = 5, .right = 5},
      .radius = 8,
      .pill_height = 35,
      .gap = 24,
      .font_family = "JetBrainsMono Nerd Font",
      .font_size = 18.0,
      .background = "#141b1e",
      .foreground = "#dadada",
      .segment = "#232a2d",
  };
  config_finalize(config);
}

/* ---------------- Lua config ---------------------------------------------- */

static bool config_path(char *out, size_t size) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  int n;

  if (xdg != NULL && xdg[0] == '/') {
    n = snprintf(out, size, "%s/debar/config.lua", xdg);
  } else if (home != NULL && home[0] != '\0') {
    n = snprintf(out, size, "%s/.config/debar/config.lua", home);
  } else {
    return false;
  }
  return n > 0 && (size_t)n < size;
}

/* Reads a pixel value (non-negative number) at stack index idx. */
static bool read_px(lua_State *L, int idx, const char *name, int *out) {
  if (lua_type(L, idx) != LUA_TNUMBER) {
    fprintf(stderr, "debar: config: '%s' must be a number, ignored\n", name);
    return false;
  }
  const double value = lua_tonumber(L, idx);
  if (!(value >= 0.0) || value > CONFIG_PADDING_MAX) {
    fprintf(stderr,
            "debar: config: '%s' must be between 0 and %d, ignored\n", name,
            CONFIG_PADDING_MAX);
    return false;
  }
  *out = (int)(value + 0.5);
  return true;
}

static void apply_position(lua_State *L, int idx, bar_config_t *config) {
  if (lua_type(L, idx) != LUA_TSTRING) {
    fprintf(stderr, "debar: config: 'position' must be a string, ignored\n");
    return;
  }

  const char *value = lua_tostring(L, idx);
  if (strcmp(value, "top") == 0) {
    config->position = BAR_POSITION_TOP;
  } else if (strcmp(value, "bottom") == 0) {
    config->position = BAR_POSITION_BOTTOM;
  } else if (strcmp(value, "left") == 0 || strcmp(value, "right") == 0) {
    fprintf(stderr,
            "debar: config: position '%s' is not supported yet "
            "(vertical bars need their own layout), using 'top'\n",
            value);
  } else {
    fprintf(stderr,
            "debar: config: unknown position '%s' "
            "(expected \"top\" or \"bottom\"), ignored\n",
            value);
  }
}

/* option = 12                      -> all four sides
 * option = { top = 4, left = 16 }  -> only the listed sides
 * Used for both 'padding' and 'margin'. */
static void apply_sides(lua_State *L, int idx, const char *option,
                        bar_sides_t *sides) {
  if (lua_type(L, idx) == LUA_TNUMBER) {
    int all = 0;
    if (read_px(L, idx, option, &all)) {
      sides->top = all;
      sides->bottom = all;
      sides->left = all;
      sides->right = all;
    }
    return;
  }

  if (lua_type(L, idx) != LUA_TTABLE) {
    fprintf(stderr,
            "debar: config: '%s' must be a number or a table, ignored\n",
            option);
    return;
  }

  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    /* key at -2, value at -1 */
    if (lua_type(L, -2) == LUA_TSTRING) {
      const char *key = lua_tostring(L, -2);
      char name[48];
      snprintf(name, sizeof(name), "%s.%s", option, key);

      if (strcmp(key, "top") == 0) {
        read_px(L, lua_gettop(L), name, &sides->top);
      } else if (strcmp(key, "bottom") == 0) {
        read_px(L, lua_gettop(L), name, &sides->bottom);
      } else if (strcmp(key, "left") == 0) {
        read_px(L, lua_gettop(L), name, &sides->left);
      } else if (strcmp(key, "right") == 0) {
        read_px(L, lua_gettop(L), name, &sides->right);
      } else {
        fprintf(stderr, "debar: config: unknown option '%s', ignored\n", name);
      }
    } else {
      fprintf(stderr, "debar: config: unexpected key in '%s', ignored\n",
              option);
    }
    lua_pop(L, 1);
  }
}

static void apply_config_table(lua_State *L, int table, bar_config_t *config) {
  lua_pushnil(L);
  while (lua_next(L, table) != 0) {
    const int value = lua_gettop(L);

    if (lua_type(L, -2) == LUA_TSTRING) {
      const char *key = lua_tostring(L, -2);
      if (strcmp(key, "position") == 0) {
        apply_position(L, value, config);
      } else if (strcmp(key, "padding") == 0) {
        apply_sides(L, value, "padding", &config->padding);
      } else if (strcmp(key, "margin") == 0) {
        apply_sides(L, value, "margin", &config->margin);
      } else {
        fprintf(stderr, "debar: config: unknown option '%s', ignored\n", key);
      }
    } else {
      fprintf(stderr, "debar: config: unexpected non-string key, ignored\n");
    }
    lua_pop(L, 1);
  }
}

static void load_lua_config(bar_config_t *config) {
  char path[1024];
  if (!config_path(path, sizeof(path))) {
    return;
  }
  if (access(path, F_OK) != 0) {
    return; /* no config file: plain defaults */
  }

  lua_State *L = luaL_newstate();
  if (L == NULL) {
    fprintf(stderr, "debar: config: cannot create Lua state\n");
    return;
  }
  luaL_openlibs(L);

  if (luaL_loadfile(L, path) != 0) {
    fprintf(stderr, "debar: config: %s\n", lua_tostring(L, -1));
  } else if (lua_pcall(L, 0, 1, 0) != 0) {
    fprintf(stderr, "debar: config: %s\n", lua_tostring(L, -1));
  } else if (lua_type(L, -1) != LUA_TTABLE) {
    fprintf(stderr,
            "debar: config: %s must end with 'return { ... }', using defaults\n",
            path);
  } else {
    apply_config_table(L, lua_gettop(L), config);
  }

  lua_close(L);
}

void config_load(bar_config_t *config) {
  config_load_defaults(config);
  load_lua_config(config);
  config_finalize(config);
}
