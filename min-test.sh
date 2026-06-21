#!/usr/bin/env bash
# min-test.sh — behavioral test for minimize/un-minimize keybindings (no anim).
# 3 windows (so there's a deck-overflow window in the tail too), then:
#   Super+Down hold -> minimize MAIN;  Super+Up hold -> un-minimize (LIFO).
# Reads maindeck-wm's LOG_EVENT/LOG_STATE to confirm behavior + the advisor's
# landmines (right window restored, focus on a visible window). Isolated headless.
set -u
RIVER=~/.local/bin/river
WM=/home/tcfialho/Documents/poc/maindeck-wm/build/maindeck-wm
HOLD=0.5   # > HOLD_THRESHOLD_MS (360ms) so wtype press+wait+release fires the hold
RT="$(mktemp -d /tmp/maindeck-min.XXXXXX)"

cat > "$RT/init.sh" <<INNER
#!/bin/sh
L="$RT/init.log"
MAINDECK_LOG=debug MAINDECK_LOG_PATH="$RT/maindeck.log" "$WM" >> "$RT/wm.log" 2>&1 &
sleep 1.0
# Bar too, so we can prove the minimized state reaches the taskbar (bar logs to stderr).
MAINDECK_LOG=debug /home/tcfialho/Documents/poc/maindeck-wm/build/maindeck-bar >> "$RT/bar.log" 2>&1 &
sleep 1.5
for n in 1 2 3; do
  echo "[init] open win\$n" >> "\$L"
  env LIBGL_ALWAYS_SOFTWARE=1 kitty --title "WIN\$n" >> "$RT/w.log" 2>&1 &
  sleep 1.5
done
sleep 1
# Hold = one wtype call: press logo, press key, sleep >360ms (-s 500), release.
echo "[init] >>> MINIMIZE (Super+Down hold)" >> "\$L"
wtype -M logo -P Down -s 500 -p Down -m logo
echo "[init] minimize sent" >> "\$L"
sleep 2
echo "[init] >>> UNMINIMIZE (Super+Up hold)" >> "\$L"
wtype -M logo -P Up -s 500 -p Up -m logo
echo "[init] unminimize sent" >> "\$L"
sleep 2
echo "[init] done" >> "\$L"
INNER
chmod +x "$RT/init.sh"

cleanup(){ [ -n "${RPID:-}" ] && kill "$RPID" 2>/dev/null; sleep 0.3; [ -n "${RPID:-}" ] && kill -9 "$RPID" 2>/dev/null; }
trap cleanup EXIT INT TERM
env XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
    "$RIVER" -c "$RT/init.sh" -log-level info > "$RT/river.log" 2>&1 &
RPID=$!
sleep 16
cleanup; trap - EXIT INT TERM

echo "=== init timeline ==="; cat "$RT/init.log"
echo ""
echo "=== WM events: minimize/unminimize + state ==="
grep -E 'minimize|unminimize|MIN\]' "$RT/maindeck.log" 2>/dev/null | head -20
echo ""
echo "=== BAR: recebeu o estado minimized do WM? (prova da cadeia WM->barra) ==="
grep -E 'taskbar: minimized=' "$RT/bar.log" 2>/dev/null
echo "(se vazio: a barra não anexou ou não recebeu — ver $RT/bar.log)"
echo ""
echo "RT=$RT  (WM: $RT/maindeck.log | BAR: $RT/bar.log)"
