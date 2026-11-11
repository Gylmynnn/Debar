project(
  'debar',
  'c',
  version: '0.5.0',
  meson_version: '>=0.60.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
# D-Bus for the StatusNotifierItem system tray.
gio = dependency('gio-2.0')
# Optional: loads PNG/SVG icons from icon themes (SVG needs librsvg's loader).
gdk_pixbuf = dependency('gdk-pixbuf-2.0', required: false)
# Typing the Wi-Fi password needs the compositor's keymap.
xkbcommon = dependency('xkbcommon')
# Lua runs ~/.config/debar/config.lua. Any of 5.1-5.4 / LuaJIT works.
lua = dependency(['lua5.4', 'lua-5.4', 'lua54', 'lua5.3', 'lua', 'luajit'])

# cos/sin in render.c need libm; some toolchains only link it transitively.
m_dep = meson.get_compiler('c').find_library('m', required: false)

tray_deps = [gio]
tray_c_args = []
if gdk_pixbuf.found()
  tray_deps += gdk_pixbuf
  tray_c_args += '-DDEBAR_HAVE_GDK_PIXBUF'
endif

wayland_scanner = find_program('wayland-scanner')
layer_shell_xml = files('protocols/wlr-layer-shell-unstable-v1.xml')
protocols_datadir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')
xdg_shell_xml = join_paths(
  protocols_datadir,
  'stable',
  'xdg-shell',
  'xdg-shell.xml',
)

layer_shell_client_header = custom_target(
  'layer-shell-client-header',
  input: layer_shell_xml,
  output: 'wlr-layer-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

layer_shell_private_code = custom_target(
  'layer-shell-private-code',
  input: layer_shell_xml,
  output: 'wlr-layer-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# wlr-layer-shell's get_popup request references xdg_popup. Generate the
# xdg-shell interface descriptors too, even though debar does not create
# popups yet, so the layer-shell protocol's object type resolves at link time.
xdg_shell_private_code = custom_target(
  'xdg-shell-private-code',
  input: xdg_shell_xml,
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

inc = include_directories('include', '.')

executable(
  'debar',
  [
    'src/main.c',
    'src/calendar.c',
    'src/config.c',
    'src/ipc.c',
    'src/jobs.c',
    'src/modules.c',
    'src/netmgr.c',
    'src/render.c',
    'src/sysinfo.c',
    'src/tray.c',
    'src/wayland.c',
    layer_shell_client_header,
    layer_shell_private_code,
    xdg_shell_private_code,
  ],
  include_directories: inc,
  c_args: tray_c_args,
  dependencies: [wayland_client, cairo, pangocairo, lua, xkbcommon, m_dep] + tray_deps,
  install: true,
)
