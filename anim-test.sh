#!/usr/bin/env bash
# anim-test.sh — autonomous runtime test for the maindeck animation patch.
#
# Boots the PATCHED river nested (wayland backend, isolated XDG_RUNTIME_DIR) with
# maindeck-wm and scripted window events, captures the temporary ANIMTRACE logs,
# and prints a verdict on: glide-vs-snap, loop-stops-at-idle, fade/scale, close.
# Does NOT touch the live session (nested + isolated; "errar sem reiniciar").
#
# Usage: ./anim-test.sh   (no args; ~14s)
set -u

RIVER="/home/tcfialho/Documents/poc/references/river/zig-out/bin/river"
WM="/home/tcfialho/Documents/poc/maindeck-wm/build/maindeck-wm"
[ -x "$RIVER" ] || { echo "FALTA river patchado: $RIVER"; exit 1; }
[ -x "$WM" ]    || { echo "FALTA maindeck-wm: $WM"; exit 1; }

# terminal for test windows
TERM_BIN=""
for t in foot kitty alacritty; do command -v "$t" >/dev/null 2>&1 && { TERM_BIN="$t"; break; }; done
[ -n "$TERM_BIN" ] || { echo "FALTA terminal (foot/kitty/alacritty)"; exit 1; }

# Headless needs no parent compositor. (Left for reference; not used.)
PARENT="${WAYLAND_DISPLAY:-none}"

RT="$(mktemp -d /tmp/maindeck-anim.XXXXXX)"
NESTED="wayland-1"   # river picks first free in the fresh dir

echo "=== maindeck anim test ==="
echo "  river  = $RIVER ($(date -r "$RIVER" +%H:%M:%S))"
echo "  wm     = $WM"
echo "  term   = $TERM_BIN   parent = $PARENT   RT = $RT"

# inner init: runs inside nested river; WAYLAND_DISPLAY is the nested one.
# Force clients into software GL so kitty (OpenGL/GLFW) survives in the nested
# compositor instead of dying with a GLFW pipe error on the GPU path.
SOFTGL="env LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe __GLX_VENDOR_LIBRARY_NAME=mesa"
cat > "$RT/init.sh" <<INNER
#!/bin/sh
L="$RT/init.log"
echo "[init] start WAYLAND_DISPLAY=\$WAYLAND_DISPLAY" >> "\$L"
# window manager
"$WM" >> "$RT/wm.log" 2>&1 &
echo "[init] wm pid \$!" >> "\$L"
sleep 1.5
# window 1 -> triggers OPEN (fade+pop). Software GL so it survives nested.
echo "[init] EVENT open win1" >> "\$L"
$SOFTGL "$TERM_BIN" >> "$RT/win.log" 2>&1 &
W1=\$!
sleep 3.0
echo "[init] win1 alive? \$(kill -0 \$W1 2>/dev/null && echo yes || echo NO-died)" >> "\$L"
# window 2 -> OPEN of win2 + win1 pushed to deck slot => MOVE + SIZE tween
echo "[init] EVENT open win2 (expect move/size tween on win1)" >> "\$L"
$SOFTGL "$TERM_BIN" >> "$RT/win.log" 2>&1 &
W2=\$!
sleep 3.0
echo "[init] win2 alive? \$(kill -0 \$W2 2>/dev/null && echo yes || echo NO-died)" >> "\$L"
# close window 2 -> CLOSE (fade-out orphan) + win1 grows back => size tween
echo "[init] EVENT close win2 (expect close fade + win1 grow)" >> "\$L"
kill \$W2 2>/dev/null
sleep 3.0
echo "[init] EVENT idle settle (expect NO further ANIMTRACE frames for ~2s)" >> "\$L"
sleep 2.5
echo "[init] done" >> "\$L"
INNER
chmod +x "$RT/init.sh"

cleanup() { [ -n "${RPID:-}" ] && kill "$RPID" 2>/dev/null; sleep 0.3; [ -n "${RPID:-}" ] && kill -9 "$RPID" 2>/dev/null; }
trap cleanup EXIT INT TERM

# launch patched river on the HEADLESS backend with anim trace on.
# WHY headless (not nested wayland): the nested wl backend ties frame callbacks
# to the parent compositor's vblank/commits, so wlr_output_schedule_frame() does
# not self-drive a continuous loop there (proved: only ~3 handleFrame calls total
# during a full run). Headless has its own software frame timer that self-drives
# regardless of commits — much closer to real DRM, and it's the standard backend
# for autonomous frame-timing tests. It also removes the GPU-client fragility.
# WLR_HEADLESS_OUTPUTS=1 creates one virtual output; pixman renders it in software.
env XDG_RUNTIME_DIR="$RT" \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
    MAINDECK_ANIM_TRACE=1 \
    "$RIVER" -no-xwayland -c "$RT/init.sh" -log-level info \
    >> "$RT/river.log" 2>&1 &
RPID=$!
echo "  nested river pid=$RPID"

# wait for nested socket
for i in $(seq 1 60); do
  [ -S "$RT/$NESTED" ] && break
  kill -0 "$RPID" 2>/dev/null || { echo "river morreu cedo"; tail -20 "$RT/river.log"; exit 1; }
  sleep 0.1
done
[ -S "$RT/$NESTED" ] || { echo "socket nested nunca apareceu"; tail -20 "$RT/river.log"; exit 1; }
echo "  nested socket up: $NESTED"

# sample river CPU (utime+stime jiffies) from /proc, in two windows:
# during the scripted animation events, then during the final idle settle.
read_cpu() { awk '{print $14+$15}' "/proc/$RPID/stat" 2>/dev/null; }
CPU_T0=$(read_cpu)
sleep 11          # events play out (open, move, close) ~ up to the idle settle
CPU_T1=$(read_cpu)
sleep 2.5         # idle settle window (no events; loop should be stopped)
CPU_T2=$(read_cpu)
echo "  river CPU jiffies: anim-phase delta=$(( ${CPU_T1:-0} - ${CPU_T0:-0} )) (11s) | idle-phase delta=$(( ${CPU_T2:-0} - ${CPU_T1:-0} )) (2.5s)"
echo "  (idle-phase delta near 0 => loop stopped at rest)"

cleanup; trap - EXIT INT TERM

echo
echo "===================== VERDICT ====================="
RL="$RT/river.log"
TRACE=$(grep -c "ANIMTRACE" "$RL" 2>/dev/null)
FRAMES=$(grep -c "ANIMTRACE frame" "$RL" 2>/dev/null)
echo "river build that ran: $(date -r "$RIVER" +%H:%M:%S)"
echo "ANIMTRACE lines total: $TRACE   (frame lines: $FRAMES)"
echo
echo "-- GLIDE check: distinct intermediate x positions per animation burst --"
grep "ANIMTRACE win" "$RL" 2>/dev/null | grep -oE 'x=-?[0-9]+' | sort -u | head -20 | tr '\n' ' '; echo
NX=$(grep "ANIMTRACE win" "$RL" 2>/dev/null | grep -oE 'x=-?[0-9]+' | sort -u | wc -l)
echo "   distinct x values seen: $NX  (>2 => glide; <=2 => snap)"
echo
echo "-- kinds observed (open/move/close) --"
grep "ANIMTRACE win" "$RL" 2>/dev/null | grep -oE 'kind=[a-z]+' | sort | uniq -c | sed 's/^/   /'
echo
echo "-- opacity range (fade) & scale range (pop) --"
echo "   op values: $(grep "ANIMTRACE win" "$RL" 2>/dev/null | grep -oE 'op=[0-9.]+' | sort -u | tr '\n' ' ')"
echo "   sc values: $(grep "ANIMTRACE win" "$RL" 2>/dev/null | grep -oE 'sc=[0-9.]+' | sort -u | tr '\n' ' ')"
echo
echo "-- IDLE check: last ANIMTRACE frame vs end of log --"
LAST_TRACE_LINE=$(grep -n "ANIMTRACE frame" "$RL" 2>/dev/null | tail -1 | cut -d: -f1)
TOTAL_LINES=$(wc -l < "$RL" 2>/dev/null)
echo "   last ANIMTRACE frame at line $LAST_TRACE_LINE of $TOTAL_LINES total"
echo "   (if many non-trace log lines follow the last frame => loop stopped => idle OK)"
echo
echo "logs kept: $RT/{river,wm,init,win}.log"
echo "==================================================="
