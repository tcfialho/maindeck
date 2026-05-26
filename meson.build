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
)

exe = executable(
  'maindeck-wm',
  'maindeck-wm.c',
  generated,
  dependencies: [
    wayland_client,
    xkbcommon,
  ],
  install: true,
)

proxy_generated = wl_mod.scan_xml(
  'protocol/wlr-foreign-toplevel-management-unstable-v1.xml',
)

executable(
  'maindeck-proxy',
  'maindeck-proxy.c',
  proxy_generated,
  dependencies: [
    wayland_client,
    cc.find_library('pthread'),
  ],
  install: true,
)

executable(
  'maindeck-toplevel-smoke',
  'tools/toplevel-smoke.c',
  proxy_generated,
  dependencies: [
    wayland_client,
  ],
  install: false,
)
