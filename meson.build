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
    # -march), seguro pra pipeline. O proxy faz relay de todo o tráfego Wayland
    # de todo app (caminho quente); o default do meson (-O0) deixava o hot path
    # sem otimização. Override com -Dbuildtype=debug pra depurar.
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

exe = executable(
  'maindeck-wm',
  'wm-ipc.c',
  'wm-log.c',
  'wm-layout.c',
  'wm-input.c',
  'wm-libinput.c',
  'wm-handlers.c',
  generated,
  dependencies: [
    wayland_client,
    xkbcommon,
  ],
  install: true,
)

proxy_generated = wl_mod.scan_xml(
  'protocol/wlr-foreign-toplevel-management-unstable-v1.xml',
  '/usr/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
)

executable(
  'maindeck-proxy',
  'proxy-process.c',
  'proxy-wire.c',
  'proxy-emit.c',
  'proxy-toplevel.c',
  'proxy-relay.c',
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
