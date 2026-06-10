# SPDX-FileCopyrightText: © 2026 Isaac Freund
# SPDX-License-Identifier: 0BSD

project(
  'maindeck-wm',
  'c',
  meson_version: '>= 1.8.0',
  version: '0.1.0',
  default_options: [
    'c_std=c99',
    'warning_level=3',
    # Default DISTRIBUÍVEL/genérico: -O2 + símbolos + LTO. Portável (sem
    # -march), seguro pra pipeline. A barra está no caminho quente do render
    # Wayland; o default do meson (-O0) deixava o hot path sem otimização.
    # Override com -Dbuildtype=debug pra depurar.
    #
    # Build local "máximo" (só esta máquina, não versionado): aplicado no build/
    # via `meson configure build -Doptimization=3 -Dc_args=-march=native
    # -Dc_link_args=-march=native`. -O3 + instruções nativas do CPU (Alder Lake:
    # AVX2/BMI). NÃO colocar -march aqui: quebraria o build genérico.
    'buildtype=debugoptimized',
    'b_lto=true',
  ],
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
	'-D_POSIX_C_SOURCE=200809L',
	'-Wno-unused-parameter',
]), language: 'c')

wayland_client = dependency('wayland-client')
xkbcommon = dependency('xkbcommon')

wl_mod = import('wayland')
generated = wl_mod.scan_xml(
  'protocol/river-window-management-v1.xml',
  'protocol/river-xkb-bindings-v1.xml',
  'protocol/river-layer-shell-v1.xml',
  'protocol/river-libinput-config-v1.xml',
  'protocol/river-input-management-v1.xml',
)

cursor_shape_protocol = wl_mod.scan_xml(
  '/usr/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml',
)
tablet_protocol = wl_mod.scan_xml(
  '/usr/share/wayland-protocols/stable/tablet/tablet-v2.xml',
)

exe = executable(
  'maindeck-wm',
  'wm-ipc.c',
  'wm-log.c',
  'wm-layout.c',
  'wm-input.c',
  'wm-libinput.c',
  'wm-handlers.c',
  'wm-config.c',
  generated,
  tablet_protocol,
  cursor_shape_protocol,
  dependencies: [
    wayland_client,
    xkbcommon,
  ],
  install: true,
)

# Foreign-toplevel protocols: window list + identifier for IPC with maindeck-wm.
# Used by the bar (taskbar) and the toplevel-smoke test tool.
toplevel_protocols = wl_mod.scan_xml(
  'protocol/wlr-foreign-toplevel-management-unstable-v1.xml',
  '/usr/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
)

cairo_dep  = dependency('cairo')
pangocairo = dependency('pangocairo')
librsvg_dep = dependency('librsvg-2.0')
dbus       = dependency('dbus-1')

bar_protocols = wl_mod.scan_xml(
  'protocol/wlr-layer-shell-unstable-v1.xml',
  '/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml',
  '/usr/share/wayland-protocols/stable/viewporter/viewporter.xml',
)
# reuse toplevel_protocols: wlr-foreign-toplevel + ext-foreign-toplevel already scanned above

executable(
  'maindeck-bar',
  'bar-main.c',
  'bar-surface.c',
  'bar-render.c',
  'bar-taskbar.c',
  'bar-quicklaunch.c',
  'bar-status.c',
  'bar-input.c',
  'bar-config.c',
  'bar-icons.c',
  'bar-tray.c',
  bar_protocols,
  toplevel_protocols,
  tablet_protocol,
  cursor_shape_protocol,
  dependencies: [
    wayland_client,
    cairo_dep,
    pangocairo,
    librsvg_dep,
    dbus,
  ],
  include_directories: include_directories('vendor'),
  install: true,
)

executable(
  'maindeck-menu',
  'maindeck-menu.c',
  'bar-icons.c',
  bar_protocols,
  tablet_protocol,
  cursor_shape_protocol,
  dependencies: [
    wayland_client,
    xkbcommon,
    cairo_dep,
    pangocairo,
    librsvg_dep,
  ],
  install: true,
)

executable(
  'maindeck-toplevel-smoke',
  'tools/toplevel-smoke.c',
  toplevel_protocols,
  dependencies: [
    wayland_client,
  ],
  install: false,
)
