#!/usr/bin/env bash
# anim-test-max.sh — does maximize/restore already animate (Tier 1), or not?
# Drives Super+Up (maximize) / Super+Down (restore) via wtype into the nested
# headless river, captures ANIMTRACE. Isolated; does not touch the live session.
set -u
RIVER="/home/tcfialho/Documents/poc/references/river/zig-out/bin/river"
WM="/home/tcfialho/Documents/poc/maindeck-wm/build/maindeck-wm"
TERM_BIN=kitty
command -v wtype >/dev/null || { echo "FALTA wtype"; exit 1; }

RT="$(mktemp -d /tmp/maindeck-max.XXXXXX)"
cat > "$RT/init.sh" <<INNER
#!/bin/sh
L="$RT/init.log"
echo "[init] start disp=\$WAYLAND_DISPLAY" >> "\$L"
"$WM" >> "$RT/wm.log" 2>&1 &
sleep 1.5
echo "[init] open win1" >> "\$L"
env LIBGL_ALWAYS_SOFTWARE=1 "$TERM_BIN" >> "$RT/win.log" 2>&1 &
sleep 3
# maximize: Super+Up
echo "[init] EVENT maximize (Super+Up)" >> "\$L"
WAYLAND_DISPLAY=\$WAYLAND_DISPLAY wtype -M logo -k Up -m logo >> "$RT/wtype.log" 2>&1
echo "[init] wtype maximize rc=\$?" >> "\$L"
sleep 3
# restore: Super+Down
echo "[init] EVENT restore (Super+Down)" >> "\$L"
WAYLAND_DISPLAY=\$WAYLAND_DISPLAY wtype -M logo -k Down -m logo >> "$RT/wtype.log" 2>&1
echo "[init] wtype restore rc=\$?" >> "\$L"
sleep 3
echo "[init] done" >> "\$L"
INNER
chmod +x "$RT/init.sh"

cleanup(){ [ -n "${RPID:-}" ] && kill "$RPID" 2>/dev/null; sleep 0.3; [ -n "${RPID:-}" ] && kill -9 "$RPID" 2>/dev/null; }
trap cleanup EXIT INT TERM

env XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
    MAINDECK_ANIM_TRACE=1 "$RIVER" -c "$RT/init.sh" -log-level info > "$RT/river.log" 2>&1 &
RPID=$!
sleep 13
cleanup; trap - EXIT INT TERM

echo "=== init events ==="
cat "$RT/init.log"
echo ""
echo "=== wtype rc (0 = tecla injetada) ==="
grep 'wtype' "$RT/init.log"
echo ""
echo "=== MAXIMIZE animou? (kind=move com x/fx progredindo após o evento maximize) ==="
echo "total win ticks: $(grep -c 'ANIMTRACE win' "$RT/river.log" 2>/dev/null)"
echo "kinds: $(grep 'ANIMTRACE win' "$RT/river.log" 2>/dev/null | grep -oE 'kind=[a-z]+' | sort | uniq -c | tr '\n' ' ')"
echo "--- amostra de ticks (procurar crescer p/ tela cheia e voltar) ---"
grep 'ANIMTRACE win' "$RT/river.log" 2>/dev/null | sed -n '1,40p'
echo ""
echo "logs: $RT/{river,wm,init,wtype}.log"
echo "RT=$RT"
