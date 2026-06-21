#!/usr/bin/env bash
# Harness isolado: river headless + maindeck-wm + 2 clientes xdg-shell + ipc_probe.py
# Valida os comandos IPC do menu de contexto (minimize/maximize/restore) e os
# prefixos +/! na notificação "windows", sem precisar da barra real nem de GUI.
set -u
ROOT=/home/tcfialho/Documents/poc/maindeck-wm
TESTDIR="$ROOT/ctxmenu-ipc-test"
PROBE="$ROOT/janelas-filhas-task/autofloat-test/probe"

# Garante o probe xdg-shell compilado (reusa o do autofloat-test).
if [ ! -x "$PROBE" ]; then
    echo "Compilando probe xdg-shell..."
    wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml "$ROOT/janelas-filhas-task/autofloat-test/xdg-shell-protocol.c"
    wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml "$ROOT/janelas-filhas-task/autofloat-test/xdg-shell-protocol.h"
    gcc -o "$PROBE" "$ROOT/janelas-filhas-task/autofloat-test/probe.c" \
        "$ROOT/janelas-filhas-task/autofloat-test/xdg-shell-protocol.c" \
        -lwayland-client -I"$ROOT/janelas-filhas-task/autofloat-test/" || { echo "probe build FAILED"; exit 1; }
fi

RT=/tmp/ctxmenu-rt
rm -rf "$RT"; mkdir -p -m 0700 "$RT"
HOME_T=/tmp/ctxmenu-home; rm -rf "$HOME_T"; mkdir -p "$HOME_T"

WMLOG="$TESTDIR/wm.log"
PROBELOG="$TESTDIR/ipc_probe.out"
rm -f "$WMLOG" "$PROBELOG"

INIT="$RT/init.sh"
cat > "$INIT" <<EOF
#!/bin/sh
export MAINDECK_LOG=debug
export MAINDECK_LOG_PATH="$WMLOG"
"$ROOT/build/maindeck-wm" > "$RT/wm-stdout.log" 2>&1 &

# Espera o socket do WM existir (ipc_init roda após o primeiro roundtrip).
i=0
while [ ! -S "$RT/maindeck-wm.sock" ] && [ \$i -lt 50 ]; do sleep 0.1; i=\$((i+1)); done

# Sobe o ipc_probe.py PRIMEIRO (faz bind no maindeck-bar.sock e fica escutando
# as notificações 'windows' antes dos clientes mapearem).
python3 "$TESTDIR/ipc_probe.py" > "$PROBELOG" 2>&1 &
PROBE_PY=\$!
sleep 0.3

# Dois clientes que ficam VIVOS durante todo o teste (o probe do autofloat-test
# sai sozinho após 2s — inútil aqui). kitty é xdg-shell e recebe identifier do WM.
kitty --config NONE -o confirm_os_window_close=0 -o background_opacity=1 \
      sh -c 'sleep 60' > "$RT/c1.log" 2>&1 &
C1=\$!
kitty --config NONE -o confirm_os_window_close=0 -o background_opacity=1 \
      sh -c 'sleep 60' > "$RT/c2.log" 2>&1 &
C2=\$!

# Espera o probe.py terminar (ele faz todos os casos e sai), com teto.
i=0
while kill -0 \$PROBE_PY 2>/dev/null && [ \$i -lt 80 ]; do sleep 0.25; i=\$((i+1)); done
kill -9 \$C1 \$C2 \$PROBE_PY 2>/dev/null || true
EOF
chmod +x "$INIT"

unset WAYLAND_DISPLAY
echo "== Rodando river headless =="
XDG_RUNTIME_DIR="$RT" HOME="$HOME_T" \
  timeout -s KILL 40 env WLR_BACKENDS=headless WLR_RENDERER=pixman \
  /home/tcfialho/.local/bin/river -no-xwayland -c "$INIT" > "$RT/river.log" 2>&1 || true

echo
echo "===== ipc_probe.out ====="
cat "$PROBELOG" 2>/dev/null || echo "(sem saída do probe)"
echo
echo "===== sinais no wm.log ====="
echo "-- IPC recebidos --"; grep -E "IPC recebeu (maximize|minimize|restore)" "$WMLOG" 2>/dev/null | head
echo "-- ctx-menu aplicados --"; grep -E "ctx-menu (maximize|minimize|restore)" "$WMLOG" 2>/dev/null | head
echo "-- identifier desconhecido (esperado p/ inválido) --"; grep -E "ctx-menu: identifier desconhecido" "$WMLOG" 2>/dev/null | head
echo "-- ERROS de protocolo / wayland (deve ser vazio) --"; grep -iE "protocol error|wl_display|fatal|assertion|error " "$WMLOG" 2>/dev/null | head
echo
echo "===== river.log (cauda — procurar crash do WM) ====="
tail -n 8 "$RT/river.log" 2>/dev/null
echo
if grep -q "^SUMMARY:" "$PROBELOG" 2>/dev/null; then
    grep "^SUMMARY:" "$PROBELOG"
fi
