#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work_dir=$(mktemp -d /tmp/maindeck-state-test.XXXXXX)
trap 'rm -rf "$work_dir"' EXIT
cc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -D_POSIX_C_SOURCE=200809L \
    -Ibuild -I. tests/wm-state-regressions.c wm-layout.c wm-libinput.c \
    wm-config.c wm-log.c wm-animation-intents.c diagnostics.c \
    build/*-protocol.c $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -Wl,--wrap=calloc -o "$work_dir/state-tests"
failed=0
for test_case in pending-parent-close floating-child-visible pending-float-focus taskbar-from-floating taskbar-floating-child minimize-child orphan-after-minimized pending-minimize node-released output-removed-fullscreen declared-parent-preserved snapshot-deduplication random-state-sequence xkb-allocation-failure pointer-allocation-failure; do
    if ASAN_OPTIONS=detect_leaks=0 "$work_dir/state-tests" "$test_case" >"$work_dir/$test_case.log" 2>&1; then
        echo "PASS: $test_case"
    else
        echo "FAIL: $test_case"
        head -35 "$work_dir/$test_case.log"
        failed=1
    fi
done
exit "$failed"
