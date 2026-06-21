#!/usr/bin/env bash
# anim-test-swap.sh — prove the swap size-tween leaves dst at 0 (auto), not frozen.
# Two windows (main+deck), then swap via Ctrl+Alt+F23 (ACTION_SWAP_MAIN_DECK),
# capturing the ANIMTRACE swap-finish dst_width. Isolated headless; safe.
set -u
RIVER="/home/tcfialho/Documents/poc/references/river/zig-out/bin/river"
WM="/home/tcfialho/Documents/poc/maindeck-wm/build/maindeck-wm"
TERM_BIN=kitty
command -v wtype >/dev/null || { echo "FALTA wtype"; exit 1; }

RT="$(mktemp -d /tmp/maindeck-swap.XXXXXX)"
cat > "$RT/init.sh" <<INNER
#!/bin/sh
L="$RT/init.log"
"$WM" >> "$RT/wm.log" 2>&1 &
sleep 1.5
echo "[init] open win1" >> "\$L"
env LIBGL_ALWAYS_SOFTWARE=1 "$TERM_BIN" >> "$RT/win.log" 2>&1 &
sleep 2.5
echo "[init] open win2 (main+deck now)" >> "\$L"
env LIBGL_ALWAYS_SOFTWARE=1 "$TERM_BIN" >> "$RT/win.log" 2>&1 &
sleep 3
echo "[init] EVENT swap (Ctrl+Alt+F23)" >> "\$L"
wtype -M ctrl -M alt -k F23 -m alt -m ctrl >> "$RT/wtype.log" 2>&1
echo "[init] swap rc=\$?" >> "\$L"
sleep 3
echo "[init] EVENT swap back" >> "\$L"
wtype -M ctrl -M alt -k F23 -m alt -m ctrl >> "$RT/wtype.log" 2>&1
sleep 3
echo "[init] done" >> "\$L"
INNER
chmod +x "$RT/init.sh"

cleanup(){ [ -n "${RPID:-}" ] && kill "$RPID" 2>/dev/null; sleep 0.3; [ -n "${RPID:-}" ] && kill -9 "$RPID" 2>/dev/null; }
trap cleanup EXIT INT TERM
env XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
    MAINDECK_ANIM_TRACE=1 "$RIVER" -c "$RT/init.sh" -log-level info > "$RT/river.log" 2>&1 &
RPID=$!
sleep 14
cleanup; trap - EXIT INT TERM

echo "=== init ==="; cat "$RT/init.log"
echo ""
echo "=== SWAP-FINISH dst_width (CRÍTICO: deve ser 0 = auto, com a correção) ==="
grep 'swap-finish' "$RT/river.log" 2>/dev/null
echo ""
echo "=== move ticks com fx (prova que o size-tween rodou: fx != 1.0 -> 1.0) ==="
grep 'ANIMTRACE' "$RT/river.log" 2>/dev/null | grep -oE 'fx=[0-9.]+' | sort -u | tr '\n' ' '; echo
echo "RT=$RT"
