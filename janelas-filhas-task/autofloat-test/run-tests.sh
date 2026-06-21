#!/usr/bin/env bash
set -e

# Criar pasta de evidências
mkdir -p janelas-filhas-task/autofloat-test/evidence

# 1. Compilar o probe
echo "Compiling probe..."
wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml janelas-filhas-task/autofloat-test/xdg-shell-protocol.c
wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml janelas-filhas-task/autofloat-test/xdg-shell-protocol.h

gcc -o janelas-filhas-task/autofloat-test/probe \
    janelas-filhas-task/autofloat-test/probe.c \
    janelas-filhas-task/autofloat-test/xdg-shell-protocol.c \
    -lwayland-client \
    -Ijanelas-filhas-task/autofloat-test/

echo "Probe compiled successfully."

run_case() {
    local caso=$1
    local mode=$2
    local extra_args=$3
    
    echo "========================================"
    echo "Running case: $caso..."
    echo "========================================"
    
    rm -rf /tmp/autofloat-rt
    mkdir -p -m 0700 /tmp/autofloat-rt
    
    export XDG_RUNTIME_DIR=/tmp/autofloat-rt
    export MAINDECK_LOG=debug
    export MAINDECK_LOG_PATH="/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-$caso.log"
    rm -f "$MAINDECK_LOG_PATH"
    
    if [ "$caso" = "huge" ]; then
        rm -rf /tmp/autofloat-home
        mkdir -p /tmp/autofloat-home/.config/maindeck
        cat > /tmp/autofloat-home/.config/maindeck/wm.json <<'JSON'
{
  "floating_app_ids": ["probe-huge"]
}
JSON
        export HOME=/tmp/autofloat-home
    else
        export HOME=/tmp/autofloat-home-empty
        rm -rf /tmp/autofloat-home-empty
        mkdir -p /tmp/autofloat-home-empty
    fi
    
    local init_script="/tmp/autofloat-init-$caso.sh"
    cat > "$init_script" <<EOF
#!/bin/sh
./build/maindeck-wm > /tmp/wm-stdout-$caso.log 2>&1 &
WM_PID=\$!

sleep 0.5

if [ "$caso" = "normal" ]; then
    ./janelas-filhas-task/autofloat-test/probe --normal > /tmp/probe1-stdout-$caso.log 2>&1 &
    P1=\$!
    ./janelas-filhas-task/autofloat-test/probe --normal > /tmp/probe2-stdout-$caso.log 2>&1 &
    P2=\$!
    sleep 2.0
    kill -9 \$P1 \$P2 2>/dev/null || true
elif [ "$caso" = "normal_relayout" ]; then
    ./janelas-filhas-task/autofloat-test/probe --normal > /tmp/probe1-stdout-$caso.log 2>&1 &
    P1=\$!
    sleep 0.5
    ./janelas-filhas-task/autofloat-test/probe --normal > /tmp/probe2-stdout-$caso.log 2>&1 &
    P2=\$!
    sleep 2.0
    kill -9 \$P1 \$P2 2>/dev/null || true
elif [ "$caso" = "slow" ]; then
    ./janelas-filhas-task/autofloat-test/probe --slow > /tmp/probe1-stdout-$caso.log 2>&1 &
    P1=\$!
    sleep 0.5
    ./janelas-filhas-task/autofloat-test/probe --normal > /tmp/probe2-stdout-$caso.log 2>&1 &
    P2=\$!
    sleep 2.5
    kill -9 \$P1 \$P2 2>/dev/null || true
    cp /tmp/probe1-stdout-$caso.log /home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/probe-slow-stdout.log 2>/dev/null || true
else
    ./janelas-filhas-task/autofloat-test/probe $mode $extra_args > /tmp/probe-stdout-$caso.log 2>&1 &
    P1=\$!
    sleep 2.0
    kill -9 \$P1 2>/dev/null || true
fi

kill -9 \$WM_PID 2>/dev/null || true
EOF
    chmod +x "$init_script"
    
    unset WAYLAND_DISPLAY
    # O river NÃO sai quando o init script termina (não é como o sway) — o
    # timeout derruba o compositor de teste depois que o caso já coletou tudo.
    timeout -s KILL 10 env WLR_BACKENDS=headless WLR_RENDERER=pixman /home/tcfialho/.local/bin/river -no-xwayland -c "$init_script" > /tmp/river-$caso.log 2>&1 || true
}

# Executar cada caso
run_case "fixed" "--fixed" "400x300"
run_case "stubborn" "--stubborn" "400x300"
run_case "normal" "--normal" ""
run_case "normal_relayout" "--normal" ""
run_case "slow" "--slow" ""
run_case "child" "--child" ""
run_case "huge" "--huge" "1800x1000"

echo "========================================"
echo "Checking Assertions..."
echo "========================================"

# Assertions for fixed
echo -n "Test fixed: "
if grep -q "window designated as floating (hint-capped)" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-fixed.log"; then
    echo "PASS"
    grep "window designated as floating (hint-capped)" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-fixed.log"
else
    echo "FAIL"
fi

# Assertions for stubborn
echo -n "Test stubborn: "
if grep -q "tile rejected by client" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-stubborn.log" && \
   grep -q "window designated as floating (tile-reject)" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-stubborn.log"; then
    echo "PASS"
    grep "tile rejected by client" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-stubborn.log"
    grep "window designated as floating (tile-reject)" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-stubborn.log"
else
    echo "FAIL"
fi

# Assertions for normal
echo -n "Test normal: "
if ! grep -q "designated as floating" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-normal.log"; then
    echo "PASS (no windows auto-floated)"
else
    echo "FAIL (some window auto-floated)"
    grep "designated as floating" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-normal.log"
fi

# Assertions for normal_relayout
echo -n "Test normal_relayout: "
if ! grep -q "tile-reject" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-normal_relayout.log"; then
    echo "PASS (no tile-reject on layout change)"
else
    echo "FAIL (tile-reject occurred)"
    grep "tile-reject" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-normal_relayout.log"
fi

# Assertions for slow: a resposta atrasada à proposta ANTERIOR não pode virar
# tile-reject (histórico de 2 propostas). O REPLAY no stdout prova que o caminho
# foi exercitado — sem ele o teste não testou nada.
echo -n "Test slow: "
if grep -q "SLOW REPLAY" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/probe-slow-stdout.log" 2>/dev/null && \
   ! grep -q "tile rejected" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-slow.log" && \
   ! grep -q "designated as floating" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-slow.log"; then
    echo "PASS (late reply to previous proposal tolerated)"
    grep "SLOW" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/probe-slow-stdout.log"
else
    echo "FAIL"
    grep "SLOW" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/probe-slow-stdout.log" 2>/dev/null || echo "(replay never happened)"
    grep -E "tile rejected|designated as floating" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-slow.log" 2>/dev/null || true
fi

# Assertions for child
echo -n "Test child: "
if grep -q "window became child" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-child.log" && \
   grep -q "child dimensions proposed" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-child.log"; then
    echo "PASS"
    grep "window became child" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-child.log"
    grep "child dimensions proposed" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-child.log"
else
    echo "FAIL"
fi

# Assertions for huge
echo -n "Test huge: "
if grep -q "floating natural dimensions proposed" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-huge.log" && \
   grep -q "floating clamped" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-huge.log"; then
    echo "PASS"
    grep "floating natural dimensions proposed" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-huge.log"
    grep "floating clamped" "/home/tcfialho/Documents/poc/maindeck-wm/janelas-filhas-task/autofloat-test/evidence/wm-huge.log"
else
    echo "FAIL"
fi
