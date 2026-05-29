#!/usr/bin/env bash
#
# test-harness.sh — Isolated nested test stack for maindeck-wm + maindeck-proxy + waybar.
#
# WHY: The live River session's only window is often the AI's terminal. Testing the
# proxy/WM live risks an EINVAL crash-loop taking the terminal (and the whole session)
# down. This harness reproduces the FULL stack (River -> proxy -> waybar -> windows)
# inside a *nested* River, in an *isolated* XDG_RUNTIME_DIR, so a crash here cannot
# touch the live session. The AI can "errar sem reiniciar".
#
# It runs:
#   nested River (wayland backend, no xwayland)   -> $RTDIR/wayland-1
#     maindeck-wm        (River window-management client)
#     maindeck-proxy     (upstream WAYLAND_DISPLAY=wayland-1) -> $RTDIR/maindeck-0
#     waybar             (WAYLAND_DISPLAY=maindeck-0)
#     N test windows (kitty/foot/alacritty) to generate foreign-toplevels
#
# Usage:
#   ./test-harness.sh [-n NUM_WINDOWS] [-d SECONDS] [-W] [-C] [-k]
#     -n NUM   number of test windows to spawn (default 2)
#     -d SECS  how long to keep the stack alive (default 8; 0 = until Ctrl-C)
#     -W       skip waybar (test only River+proxy+WM+windows)
#     -C       use the user's real waybar config instead of the minimal one
#     -k       keep $RTDIR (logs) after exit instead of deleting it
#     -b DIR   binary dir for maindeck-wm/maindeck-proxy (default ./build)
#     -p NAME  parent compositor socket to nest inside (wayland-N or abs path).
#              Default: auto-detect the outermost real compositor (never the proxy).
#
# After it runs it prints a VERDICT grepped from the logs:
#   - waybar EINVAL / configure-timeout  (the bug)
#   - proxy malformed/collision diagnostics
#   - whether the nested socket + proxy socket came up
#
set -u

# ---- args ----
NUM=2; DUR=8; SKIP_WAYBAR=0; REAL_CONFIG=0; KEEP=0; PARENT_OVERRIDE=""; WINS_FIRST=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINDIR="$SCRIPT_DIR/build"
while getopts "n:d:WCkb:p:B" opt; do
  case "$opt" in
    n) NUM="$OPTARG" ;;
    d) DUR="$OPTARG" ;;
    W) SKIP_WAYBAR=1 ;;
    C) REAL_CONFIG=1 ;;
    k) KEEP=1 ;;
    b) BINDIR="$OPTARG" ;;
    p) PARENT_OVERRIDE="$OPTARG" ;;
    B) WINS_FIRST=1 ;;   # open windows BEFORE waybar (mirrors live 'configure timeout' path)
    *) sed -n '2,33p' "$0"; exit 2 ;;
  esac
done

WM_BIN="$BINDIR/maindeck-wm"
PROXY_BIN="$BINDIR/maindeck-proxy"
[ -x "$WM_BIN" ]    || WM_BIN="$(command -v maindeck-wm || true)"
[ -x "$PROXY_BIN" ] || PROXY_BIN="$(command -v maindeck-proxy || true)"
# The smoke tool is a test-only binary (not installed); always prefer the build
# dir so the positive test works even when -b points at ~/.local/bin.
SMOKE_BIN="$SCRIPT_DIR/build/maindeck-toplevel-smoke"
[ -x "$SMOKE_BIN" ] || SMOKE_BIN="$BINDIR/maindeck-toplevel-smoke"
[ -x "$SMOKE_BIN" ] || SMOKE_BIN="$(command -v maindeck-toplevel-smoke || true)"
if [ ! -x "$WM_BIN" ] || [ ! -x "$PROXY_BIN" ]; then
  echo "ERRO: binarios nao encontrados. Compile: ninja -C build" >&2
  echo "  WM_BIN=$WM_BIN PROXY_BIN=$PROXY_BIN" >&2
  exit 1
fi

# pick a terminal that actually exists on this machine
TERM_BIN=""
for t in foot kitty alacritty weston-terminal; do
  if command -v "$t" >/dev/null 2>&1; then TERM_BIN="$t"; break; fi
done

if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
  echo "ERRO: XDG_RUNTIME_DIR nao definido." >&2
  exit 1
fi

# Pick the PARENT compositor socket to nest inside.
# IMPORTANT: do NOT nest inside maindeck-proxy (that double-proxies and contaminates
# the test). Default to the outermost real compositor: a wayland-N socket whose
# listener is a known compositor (niri/river/sway/...) and NOT maindeck-proxy.
# This mirrors how the live River nests inside its own parent.
pick_parent() {
  # explicit override wins
  if [ -n "$PARENT_OVERRIDE" ]; then
    case "$PARENT_OVERRIDE" in
      /*) echo "$PARENT_OVERRIDE" ;;
      *)  echo "$XDG_RUNTIME_DIR/$PARENT_OVERRIDE" ;;
    esac
    return
  fi
  # find listeners for each wayland-* socket via ss; prefer a real compositor
  local best=""
  for s in "$XDG_RUNTIME_DIR"/wayland-*; do
    [ -S "$s" ] || continue
    case "$s" in *.lock) continue;; esac
    local who
    who="$(ss -xlp 2>/dev/null | grep -F "$s " | grep -oE 'users:\(\("[^"]+"' | head -1 | sed -E 's/.*\("//')"
    case "$who" in
      maindeck-proxy|"") continue ;;            # never nest inside our proxy
      niri|river|sway|labwc|wayfire|hyprland|Hyprland|kwin_wayland|gnome-shell|weston|mutter)
        echo "$s"; return ;;                     # a real compositor: use it
      *) [ -z "$best" ] && best="$s" ;;          # fallback candidate
    esac
  done
  # fallback: current WAYLAND_DISPLAY if it isn't the proxy, else best guess
  if [ -n "${WAYLAND_DISPLAY:-}" ]; then
    case "$WAYLAND_DISPLAY" in
      maindeck-*) : ;;
      *) [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && { echo "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"; return; } ;;
    esac
  fi
  echo "$best"
}

PARENT_SOCK="$(pick_parent)"
if [ -z "$PARENT_SOCK" ] || [ ! -S "$PARENT_SOCK" ]; then
  echo "ERRO: nao encontrei socket de compositor pai. Use -p <wayland-N|caminho>." >&2
  echo "  Sockets disponiveis:" >&2
  ss -xlp 2>/dev/null | grep -E "wayland-" | sed 's/^/    /' >&2
  exit 1
fi

# ---- isolated runtime dir ----
RTDIR="$(mktemp -d /tmp/maindeck-harness.XXXXXX)"
ln -s "$PARENT_SOCK" "$RTDIR/wayland-parent"
NESTED_DISPLAY="wayland-1"          # River picks the first free; in a fresh dir that's wayland-1
PROXY_DISPLAY="maindeck-0"          # proxy picks the first free maindeck-N; fresh dir -> maindeck-0

echo "=== maindeck test harness ==="
echo "  RTDIR        = $RTDIR"
echo "  parent       = $PARENT_SOCK"
echo "  WM_BIN       = $WM_BIN"
echo "  PROXY_BIN    = $PROXY_BIN"
echo "  terminal     = ${TERM_BIN:-<none found>}"
echo "  windows      = $NUM   duration = ${DUR}s   waybar = $([ $SKIP_WAYBAR = 1 ] && echo no || echo yes)"
echo

# ---- minimal waybar config (taskbar-focused, fast) unless -C ----
WAYBAR_ARGS=()
if [ $SKIP_WAYBAR = 0 ]; then
  if [ $REAL_CONFIG = 0 ]; then
    cat > "$RTDIR/waybar-config.jsonc" <<'JSON'
{
  "layer": "top",
  "position": "bottom",
  "height": 32,
  "modules-left": ["wlr/taskbar"],
  "modules-center": [],
  "modules-right": ["clock"],
  "wlr/taskbar": {
    "all-outputs": true,
    "format": "{title}",
    "on-click": "activate"
  }
}
JSON
    cat > "$RTDIR/waybar-style.css" <<'CSS'
* { font-size: 12px; }
CSS
    WAYBAR_ARGS=(-c "$RTDIR/waybar-config.jsonc" -s "$RTDIR/waybar-style.css")
  fi
fi

# ---- inner init: runs INSIDE the nested River ----
INIT="$RTDIR/init.sh"
cat > "$INIT" <<INNER
#!/bin/sh
# Runs inside nested River. XDG_RUNTIME_DIR + WAYLAND_DISPLAY are already
# set by River to the NESTED display ($NESTED_DISPLAY) for its children.
log() { echo "[init \$(date +%H:%M:%S.%3N)] \$*" >> "$RTDIR/init.log"; }
log "inner init start; WAYLAND_DISPLAY=\$WAYLAND_DISPLAY XDG_RUNTIME_DIR=\$XDG_RUNTIME_DIR"

# 1) window manager (River's management client)
"$WM_BIN" >> "$RTDIR/wm.log" 2>&1 &
log "maindeck-wm started pid \$!"
sleep 0.3

# 2) proxy: upstream = nested River ($NESTED_DISPLAY); it will create $PROXY_DISPLAY
WAYLAND_DISPLAY="$NESTED_DISPLAY" "$PROXY_BIN" >> "$RTDIR/proxy.log" 2>&1 &
log "maindeck-proxy started pid \$!"

# wait for proxy socket
i=0
while [ \$i -lt 100 ]; do
  [ -S "$RTDIR/$PROXY_DISPLAY" ] && break
  i=\$((i+1)); sleep 0.1
done
if [ -S "$RTDIR/$PROXY_DISPLAY" ]; then
  log "proxy socket up: $PROXY_DISPLAY"
else
  log "WARN: proxy socket $PROXY_DISPLAY never appeared"
fi

spawn_windows() {
  n=0
  while [ \$n -lt $NUM ]; do
    if [ -n "$TERM_BIN" ]; then
      WAYLAND_DISPLAY="$NESTED_DISPLAY" "$TERM_BIN" >> "$RTDIR/win.log" 2>&1 &
      log "spawned $TERM_BIN #\$n pid \$!"
    fi
    n=\$((n+1)); sleep 0.4
  done
}

start_waybar() {
  if [ "$SKIP_WAYBAR" = "0" ]; then
    WAYLAND_DISPLAY="$PROXY_DISPLAY" GDK_BACKEND=wayland waybar ${WAYBAR_ARGS[*]} >> "$RTDIR/waybar.log" 2>&1 &
    log "waybar started pid \$! (display=$PROXY_DISPLAY)"
  fi
}

# Order: with -B, windows exist BEFORE waybar (mirrors the live session where
# the AI terminal is already open -> 'configure timeout' path). Otherwise
# waybar first, then windows (the 'error reading events' path).
if [ "$WINS_FIRST" = "1" ]; then
  spawn_windows
  sleep 0.4
  start_waybar
else
  start_waybar
  spawn_windows
fi

# 5) POSITIVE TEST: a headless foreign-toplevel client through the proxy, to
#    measure whether output_enter actually reaches clients (taskbar buttons).
#    'no EINVAL' alone does NOT prove the taskbar works — with_output does.
#    Retry until we observe all $NUM toplevels (real waybar config + many
#    windows can be slow to settle), so the verdict is deterministic.
if [ -x "$SMOKE_BIN" ]; then
  s=0
  while [ \$s -lt 12 ]; do
    [ -S "$RTDIR/$PROXY_DISPLAY" ] || { s=\$((s+1)); sleep 0.4; continue; }
    WAYLAND_DISPLAY="$PROXY_DISPLAY" "$SMOKE_BIN" > "$RTDIR/smoke-proxy.txt" 2>&1
    got=\$(grep -oE 'toplevels=[0-9]+' "$RTDIR/smoke-proxy.txt" 2>/dev/null | cut -d= -f2)
    [ "\${got:-0}" -ge "$NUM" ] && break
    s=\$((s+1)); sleep 0.5
  done
  log "smoke(proxy) -> \$(grep ^summary "$RTDIR/smoke-proxy.txt" 2>/dev/null)"
fi
# baseline: same probe straight against nested River (expect with_output=0 on River 0.4.5)
if [ -x "$SMOKE_BIN" ]; then
  WAYLAND_DISPLAY="$NESTED_DISPLAY" "$SMOKE_BIN" > "$RTDIR/smoke-river.txt" 2>&1
  log "smoke(river) -> \$(grep ^summary "$RTDIR/smoke-river.txt" 2>/dev/null)"
fi

log "all components launched"
wait
INNER
chmod +x "$INIT"

# ---- cleanup ----
RIVER_PID=""
cleanup() {
  [ -n "$RIVER_PID" ] && kill "$RIVER_PID" 2>/dev/null
  # give River a moment to tear down its children, then hard-kill stragglers by RTDIR
  sleep 0.3
  [ -n "$RIVER_PID" ] && kill -9 "$RIVER_PID" 2>/dev/null
  if [ $KEEP = 1 ]; then
    echo "(logs kept in $RTDIR)"
  else
    rm -rf "$RTDIR"
  fi
}
trap cleanup EXIT INT TERM

# ---- launch nested River ----
env XDG_RUNTIME_DIR="$RTDIR" WAYLAND_DISPLAY="wayland-parent" \
    WLR_BACKENDS=wayland \
    river -no-xwayland -c "$INIT" -log-level info \
    >> "$RTDIR/river.log" 2>&1 &
RIVER_PID=$!
echo "nested River pid=$RIVER_PID"

# wait for nested socket
for i in $(seq 1 50); do
  [ -S "$RTDIR/$NESTED_DISPLAY" ] && break
  kill -0 "$RIVER_PID" 2>/dev/null || { echo "ERRO: River morreu antes de criar socket"; tail -15 "$RTDIR/river.log"; exit 1; }
  sleep 0.1
done
if [ -S "$RTDIR/$NESTED_DISPLAY" ]; then
  echo "nested socket up: $NESTED_DISPLAY"
else
  echo "ERRO: nested socket nunca apareceu"; tail -15 "$RTDIR/river.log"; exit 1
fi

# ---- run for DUR seconds (or until Ctrl-C if DUR=0) ----
if [ "$DUR" = "0" ]; then
  echo "Stack rodando. Ctrl-C para encerrar. Logs: $RTDIR/*.log"
  wait "$RIVER_PID"
else
  echo "Stack rodando por ${DUR}s..."
  end=$(( $(date +%s) + DUR ))
  while [ "$(date +%s)" -lt "$end" ]; do
    kill -0 "$RIVER_PID" 2>/dev/null || { echo "River saiu cedo."; break; }
    sleep 0.5
  done
fi

# ---- verdict ----
echo
echo "===================== VERDICT ====================="
WB="$RTDIR/waybar.log"; PX="$RTDIR/proxy.log"
if [ $SKIP_WAYBAR = 0 ] && [ -f "$WB" ]; then
  EINVAL=$(grep -c -iE "Error 22|EINVAL|Argumento inv|Invalid argument|Timed out waiting for initial .configure|Error reading events from display" "$WB" 2>/dev/null)
  STARTS=$(grep -c -iE "Using configuration file|Bar configured" "$WB" 2>/dev/null)
  echo "waybar: config-loads=$STARTS  EINVAL/configure-timeout hits=$EINVAL"
  if [ "${EINVAL:-0}" -gt 0 ]; then
    echo "  >>> BUG REPRODUCED: waybar hit EINVAL/configure timeout <<<"
    grep -iE "Error 22|Timed out|EINVAL" "$WB" | head -4 | sed 's/^/    /'
  else
    echo "  >>> waybar survived (no EINVAL/configure timeout) <<<"
  fi
fi
if [ -f "$PX" ]; then
  echo "proxy diagnostics:"
  grep -iE "collision|malformed|wl_display.error|send\(dst.*failed|ending src|dirB: (injected|replayed)|SENDING synthetic" "$PX" 2>/dev/null | tail -8 | sed 's/^/    /'
fi

# ---- positive test: did output_enter reach clients through the proxy? ----
SP="$RTDIR/smoke-proxy.txt"; SR="$RTDIR/smoke-river.txt"
echo "TASKBAR (output_enter) test:"
if [ -f "$SR" ]; then
  echo "  via River direct : $(grep '^summary' "$SR" 2>/dev/null || cat "$SR" | head -1)"
fi
if [ -f "$SP" ]; then
  SUM="$(grep '^summary' "$SP" 2>/dev/null)"
  echo "  via proxy        : ${SUM:-$(head -1 "$SP")}"
  PT=$(printf '%s' "$SUM" | grep -oE 'toplevels=[0-9]+' | cut -d= -f2)
  PW=$(printf '%s' "$SUM" | grep -oE 'with_output=[0-9]+' | cut -d= -f2)
  if [ -n "${PW:-}" ] && [ -n "${PT:-}" ] && [ "${PT:-0}" -gt 0 ] && [ "${PW:-0}" = "${PT:-0}" ]; then
    echo "  >>> PASS: all $PT toplevels have output_enter via proxy (taskbar will show buttons) <<<"
  elif [ "${PW:-0}" = "0" ]; then
    echo "  >>> FAIL: 0/${PT:-?} toplevels have output_enter — taskbar would be EMPTY <<<"
  else
    echo "  >>> PARTIAL: ${PW:-?}/${PT:-?} toplevels have output_enter <<<"
  fi
fi
echo "logs: $RTDIR/{river,wm,proxy,waybar,init,win}.log  smoke: smoke-{proxy,river}.txt"
echo "==================================================="
