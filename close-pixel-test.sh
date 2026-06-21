#!/usr/bin/env bash
# close-pixel-test.sh — PROVES the occlusion fix by pixels, not by trace.
# 1 MAIN + 2 DECK, close the visible deck (Super+Tab then Super+Delete), and
# grab the framebuffer ~90ms into the 200ms close fade. With the fix, the deck
# slot shows the fading orphan composited over the rising substitute; without
# it, the substitute is fully opaque there.
# Pass the river binary as $1 (instrumented debug build, or a baseline).
set -u
RIVER="${1:-/home/tcfialho/Documents/poc/references/river/zig-out/bin/river}"
WM=/home/tcfialho/Documents/poc/maindeck-wm/build/maindeck-wm
TAG="${2:-fix}"
RT="$(mktemp -d /tmp/maindeck-px.XXXXXX)"
SHOT="/tmp/close-${TAG}.png"

cat > "$RT/init.sh" <<INNER
#!/bin/sh
L="$RT/init.log"
MAINDECK_LOG=debug MAINDECK_LOG_PATH="$RT/maindeck.log" "$WM" >> "$RT/wm.log" 2>&1 &
sleep 1.5
for n in 1 2 3; do
  env LIBGL_ALWAYS_SOFTWARE=1 kitty --title "WIN\$n" >> "$RT/w.log" 2>&1 &
  sleep 1.5
done
sleep 1
wtype -M logo -k Tab -m logo            # target -> visible deck (idx 1)
sleep 1
# Close, then immediately grab mid-fade (~90ms into the 200ms fade).
( sleep 0.09; WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR="$RT" grim "$SHOT" 2>>"$RT/grim.log"; echo "[init] grab rc=\$?" >> "\$L" ) &
wtype -M logo -k Delete -m logo
echo "[init] close sent" >> "\$L"
sleep 2
echo "[init] done" >> "\$L"
INNER
chmod +x "$RT/init.sh"

cleanup(){ [ -n "${RPID:-}" ] && kill "$RPID" 2>/dev/null; sleep 0.3; [ -n "${RPID:-}" ] && kill -9 "$RPID" 2>/dev/null; }
trap cleanup EXIT INT TERM
env XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
    "$RIVER" -c "$RT/init.sh" -log-level error > "$RT/river.log" 2>&1 &
RPID=$!
sleep 14
cleanup; trap - EXIT INT TERM

echo "=== grab status ==="; cat "$RT/grim.log" 2>/dev/null; ls -la "$SHOT" 2>/dev/null
echo "RT=$RT  SHOT=$SHOT"
