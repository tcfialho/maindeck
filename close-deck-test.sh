#!/usr/bin/env bash
# close-deck-test.sh — reproduces the user's case: 1 MAIN + 2 DECK, close the
# VISIBLE deck window (index 1). Captures the MDANIM trace from the instrumented
# river to see whether spawnClose fires (orphan) or drops (empty snapshot).
#   target toggle = Super+Tab (tap); close target = Super+Delete.
set -u
RIVER=/home/tcfialho/Documents/poc/references/river/zig-out/bin/river   # instrumented debug build
WM=/home/tcfialho/Documents/poc/maindeck-wm/build/maindeck-wm
RT="$(mktemp -d /tmp/maindeck-closedeck.XXXXXX)"

cat > "$RT/init.sh" <<INNER
#!/bin/sh
L="$RT/init.log"
MAINDECK_LOG=debug MAINDECK_LOG_PATH="$RT/maindeck.log" "$WM" >> "$RT/wm.log" 2>&1 &
sleep 1.5
for n in 1 2 3; do
  echo "[init] open WIN\$n" >> "\$L"
  env LIBGL_ALWAYS_SOFTWARE=1 kitty --title "WIN\$n" >> "$RT/w.log" 2>&1 &
  sleep 1.5
done
sleep 1
# Now: WIN3=MAIN(0), WIN2=DECK visible(1), WIN1=hidden(2).
# Move target to the visible deck (Super+Tab tap), then close it (Super+Delete).
echo "[init] >>> toggle target to DECK (Super+Tab)" >> "\$L"
wtype -M logo -k Tab -m logo
sleep 1
echo "[init] >>> CLOSE visible deck (Super+Delete)" >> "\$L"
wtype -M logo -k Delete -m logo
echo "[init] close sent" >> "\$L"
sleep 2
echo "[init] done" >> "\$L"
INNER
chmod +x "$RT/init.sh"

cleanup(){ [ -n "${RPID:-}" ] && kill "$RPID" 2>/dev/null; sleep 0.3; [ -n "${RPID:-}" ] && kill -9 "$RPID" 2>/dev/null; }
trap cleanup EXIT INT TERM
env XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
    "$RIVER" -c "$RT/init.sh" -log-level info > "$RT/river.log" 2>&1 &
RPID=$!
sleep 15
cleanup; trap - EXIT INT TERM

echo "=== init timeline ==="; cat "$RT/init.log"
echo ""
echo "=== WM: state at close (qual janela era o target/deck?) ==="
grep -E 'CLOSE|close|target=|MAIN|DECK|key pressed' "$RT/maindeck.log" 2>/dev/null | tail -20
echo ""
echo "=== ⭐ MDANIM: o close animou (OK orphan) ou fechou a seco (DROP / nem chamou)? ==="
grep -E 'MDANIM' "$RT/river.log" 2>/dev/null
echo ""
echo "RT=$RT"
