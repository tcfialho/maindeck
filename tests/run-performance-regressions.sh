#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work_dir=$(mktemp -d /tmp/maindeck-perf-test.XXXXXX)
trap 'rm -rf "$work_dir"' EXIT
cc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -D_POSIX_C_SOURCE=200809L -I. -Ibuild tests/bar-render-regressions.c bar-render.c diagnostics.c \
    $(pkg-config --cflags --libs cairo pangocairo wayland-client) -o "$work_dir/render"
ASAN_OPTIONS=detect_leaks=0 "$work_dir/render"
cc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -I. tests/icon-loader-regressions.c diagnostics.c \
    $(pkg-config --cflags --libs cairo librsvg-2.0) -pthread -o "$work_dir/icons"
timeout 20 env ASAN_OPTIONS=detect_leaks=0 "$work_dir/icons" "$work_dir"
