-- ~/.config/debar/config.lua
--
-- Copy this file to ~/.config/debar/config.lua (or $XDG_CONFIG_HOME/debar/).
-- It is plain Lua and must end with `return { ... }`.
-- Every option is optional: leave one out and the default is used.
-- Restart debar after editing.

return {
  -- Which screen edge the bar sits on: "top" or "bottom".
  -- ("left" / "right" are not supported yet.)
  position = "top",

  -- Space between the bar's edges and the pills inside it, in px.
  -- The bar height follows from it: 35 (pill) + top + bottom.
  -- A single number sets all four sides: padding = 12
  padding = {
    top    = 9,
    bottom = 9,
    left   = 24,
    right  = 24,
  },

  -- Gap between the bar and the screen edges, in px (also a single number
  -- or a partial table). For a top bar, `top` is the distance from the
  -- screen edge and `bottom` is the space kept free between the bar and the
  -- windows; for a bottom bar it is the other way around.
  margin = {
    top    = 6,
    bottom = 6,
    left   = 5,
    right  = 5,
  },
}
