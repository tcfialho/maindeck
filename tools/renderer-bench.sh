#!/bin/bash
#
# Benchmark de CPU do river por renderer, sem tocar a sessão viva.
#
# Sobe um river aninhado (backend headless) rodando maindeck-wm como WM,
# conecta um kitty animando ~30fps e mede o CPU do river aninhado.
#
# Uso:
#   tools/renderer-bench.sh [renderer] [segundos]
#     renderer: gles2 | vulkan | pixman | "" (default do binário)
#     segundos: janela de medição (default 20)
#   RIVER_BIN=/caminho/para/river para testar um build específico.
#
# Resultado de referência (RTX 4050, driver 610.43.02, wlroots 0.20.1,
# kitty ~30fps, 2026-06-11):
#   gles2:  ~8.1% de CPU do river  (busy-wait do driver NVIDIA em eglMakeCurrent)
#   vulkan: ~0.75% de CPU do river

set -u

R="${1-}"
DUR="${2:-20}"
RIVER_BIN="${RIVER_BIN:-$HOME/.local/bin/river}"
WM_BIN="${WM_BIN:-$HOME/.local/bin/maindeck-wm}"
LOG="/tmp/river-bench-${R:-default}.log"
SOCK_FILE=$(mktemp /tmp/renderer-bench-sock.XXXXXX)

cleanup() {
    [ -n "${KPID-}" ] && kill "$KPID" 2>/dev/null
    [ -n "${RPID-}" ] && kill "$RPID" 2>/dev/null
    rm -f "$SOCK_FILE"
}
trap cleanup EXIT

env -u WAYLAND_DISPLAY -u DISPLAY -u WLR_RENDERER \
    ${R:+WLR_RENDERER=$R} WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    "$RIVER_BIN" -no-xwayland -log-level error \
    -c "echo \$WAYLAND_DISPLAY > $SOCK_FILE; exec $WM_BIN" >"$LOG" 2>&1 &
RPID=$!
sleep 2.5
kill -0 "$RPID" 2>/dev/null || { echo "river (${R:-default}) morreu:"; tail -5 "$LOG"; exit 1; }

SOCK=$(cat "$SOCK_FILE" 2>/dev/null)
[ -z "$SOCK" ] && { echo "não capturei o socket do river aninhado"; exit 1; }
# Nunca conectar clientes de teste na sessão viva.
[ "$SOCK" = "${WAYLAND_DISPLAY:-wayland-1}" ] && { echo "socket é o da sessão viva, abortando"; exit 1; }
echo "renderer=${R:-default} river_pid=$RPID socket=$SOCK"

env -u DISPLAY WAYLAND_DISPLAY="$SOCK" kitty --config NONE -o allow_remote_control=no \
    sh -c 'while :; do date +%N; sleep 0.03; done' >/dev/null 2>&1 &
KPID=$!
sleep 4
kill -0 "$KPID" 2>/dev/null || { echo "kitty não subiu no river aninhado"; exit 1; }

U0=$(awk '{print $14+$15}' "/proc/$RPID/stat")
K0=$(awk '{print $14+$15}' "/proc/$KPID/stat")
sleep "$DUR"
U1=$(awk '{print $14+$15}' "/proc/$RPID/stat")
K1=$(awk '{print $14+$15}' "/proc/$KPID/stat")

awk -v t=$((U1 - U0)) -v k=$((K1 - K0)) -v d="$DUR" -v hz="$(getconf CLK_TCK)" -v r="${R:-default}" \
    'BEGIN{printf "RESULT renderer=%s river_cpu=%.2f%% kitty_cpu=%.2f%% (janela de %ds)\n", r, t/hz/d*100, k/hz/d*100, d}'
