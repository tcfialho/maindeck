#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

assert_contains() {
    local file="$1"
    local pattern="$2"
    local message="$3"
    if ! grep -Eq "$pattern" "$ROOT/$file"; then
        fail "$message"
    fi
}

assert_no_set_shape_in_func() {
    local file="$1"
    local func="$2"
    local message="$3"
    awk -v fn="$func" '
        $0 ~ "static void " fn "\\(" { in_func = 1 }
        in_func && /wp_cursor_shape_device_v1_set_shape/ { bad = 1 }
        in_func && /^}/ { in_func = 0 }
        END { exit bad ? 1 : 0 }
    ' "$ROOT/$file" || fail "$message"
}

assert_contains meson.build 'cursor-shape/cursor-shape-v1\.xml' \
    'cursor-shape protocol must be generated'
assert_contains meson.build 'stable/tablet/tablet-v2\.xml' \
    'tablet-v2 protocol must be linked for cursor-shape tablet interface references'

assert_contains bar-main.c 'wp_cursor_shape_manager_v1_get_pointer' \
    'bar must create a cursor-shape device for its wl_pointer'
assert_contains maindeck-menu.c 'wp_cursor_shape_manager_v1_get_pointer' \
    'menu must create a cursor-shape device for its wl_pointer'
assert_contains wm-input.c 'wp_cursor_shape_manager_v1_get_pointer' \
    'wm must create a cursor-shape device for the river seat wl_pointer'

assert_contains bar-input.c 'surf == bar->wl_surface' \
    'bar pointer enter must distinguish the real bar surface'
assert_contains bar-input.c 'surf == bar->bg_surface' \
    'bar pointer enter must distinguish the background input surface'
assert_contains bar-input.c 'bar->ptr_surface != bar->wl_surface' \
    'bar pointer motion/button must ignore non-bar surfaces'

assert_contains wm-input.c 'seat_handle_wl_seat' \
    'wm must handle river_seat_v1.wl_seat to map the corresponding wl_seat'
assert_contains wm-input.c 'river_seat_v1_set_xcursor_theme' \
    'wm must configure the compositor cursor theme for the seat'
assert_contains wm-input.c 'WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT' \
    'wm must install a remembered default cursor shape'

assert_no_set_shape_in_func bar-input.c ptr_motion \
    'bar must not set cursor shape in pointer motion hot path'
assert_no_set_shape_in_func maindeck-menu.c pointer_motion \
    'menu must not set cursor shape in pointer motion hot path'

printf 'PASS: cursor fallback/static routing checks\n'
