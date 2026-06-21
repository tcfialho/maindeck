#!/usr/bin/env bash
# grow-test.sh — proves the lone-window clip-reveal GATE (MDCLIP trace):
#   A) 2 windows, close the deck -> main is alone and grows  => clip-reveal ON
#   B) 3 windows, swap main<->deck (2 visible)               => must NOT fire
# Fail-safe: clip-reveal may only arm when visibleCount==1.
set -u
RIVER=/home/tcfialho/Documents/poc/references/river/zig-out/bin/river
WM=/home/tcfialho/Documents/poc/maindeck-wm/build/maindeck-wm
RT="$(mktemp -d /tmp/maindeck-grow.XXXXXX)"

cat > "$RT/init.sh" <<INNER
#!/bin/sh
L="$RT/init.log"
MAINDECK_LOG=debug MAINDECK_LOG_PATH="$RT/maindeck.log" "$WM" >> "$RT/wm.log" 2>&1 &
sleep 1.5
echo "[A] open 2 windows" >> "\$L"
env LIBGL_ALWAYS_SOFTWARE=1 kitty --title WIN1 >> "$RT/w.log" 2>&1 & sleep 1.5
env LIBGL_ALWAYS_SOFTWARE=1 kitty --title WIN2 >> "$RT/w.log" 2>&1 & sleep 1.5
echo "[A] >>> close visible DECK" >> "\$L"
wtype -M logo -k Tab -m logo ; sleep 0.8
wtype -M logo -k Delete -m logo
echo "[A] closed deck -> main alone should grow" >> "\$L"
sleep 2.5
echo "[B] open 2 more (=3), then swap" >> "\$L"
env LIBGL_ALWAYS_SOFTWARE=1 kitty --title WIN3 >> "$RT/w.log" 2>&1 & sleep 1.5
env LIBGL_ALWAYS_SOFTWARE=1 kitty --title WIN4 >> "$RT/w.log" 2>&1 & sleep 1.5
echo "[B] >>> swap (Super+Tab HOLD)" >> "\$L"
wtype -M logo -P Tab -s 500 -p Tab -m logo
echo "[B] swapped -> clip-reveal must NOT arm" >> "\$L"
sleep 2
echo "[init] done" >> "\$L"
INNER
chmod +x "$RT/init.sh"

cleanup(){ [ -n "${RPID:-}" ] && kill "$RPID" 2>/dev/null; sleep 0.3; [ -n "${RPID:-}" ] && kill -9 "$RPID" 2>/dev/null; }
trap cleanup EXIT INT TERM
env XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
    "$RIVER" -c "$RT/init.sh" -log-level info > "$RT/river.log" 2>&1 &
RPID=$!
sleep 18
cleanup; trap - EXIT INT TERM

echo "=== timeline ==="; cat "$RT/init.log"
echo ""
echo "=== MDCLIP trace (gate) — A should be ON(count=1), B off(count=2) ==="
grep -E 'MDCLIP' "$RT/river.log" 2>/dev/null
echo ""
echo "=== crash check (clip-reveal must not panic) ==="
grep -ciE 'panic|assertion|segfault|error:' "$RT/river.log" 2>/dev/null
echo "RT=$RT"
