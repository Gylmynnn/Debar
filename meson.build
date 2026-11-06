project(
  'debar',
  'c',
  version: '0.1.0',
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
    'src/config.c',
    'src/ipc.c',
    'src/modules.c',
    'src/render.c',
    'src/wayland.c',
    layer_shell_client_header,
    layer_shell_private_code,
    xdg_shell_private_code,
  ],
  include_directories: inc,
  dependencies: [wayland_client, cairo, pangocairo],
  install: true,
)
