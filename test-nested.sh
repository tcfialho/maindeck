#!/bin/sh
# Testa maindeck-wm em uma sessão River aninhada.
# Rode de dentro de uma sessão Wayland ativa.
# Uso: ./test-nested.sh [num_janelas]
#
# Log em tempo real: tail -f ~/.local/state/maindeck/maindeck.log

set -e

if [ -z "$WAYLAND_DISPLAY" ]; then
    echo "Erro: WAYLAND_DISPLAY não definido. Rode dentro de uma sessão Wayland." >&2
    exit 1
fi

BINARY="/home/tcfialho/.local/bin/maindeck-wm"
LOG="$HOME/.local/state/maindeck/maindeck.log"
N="${1:-3}"

if [ ! -x "$BINARY" ]; then
    echo "Erro: $BINARY não encontrado. Compile primeiro com: meson compile -C build" >&2
    exit 1
fi

mkdir -p "$(dirname "$LOG")"
echo "=== teste aninhado $(date) ===" >> "$LOG"

# Init script para o River aninhado
INIT=$(mktemp /tmp/maindeck-test-init.XXXXXX.sh)
cat > "$INIT" << INNEREOF
#!/bin/sh
"$BINARY" &
sleep 0.3
i=0
while [ \$i -lt $N ]; do
    foot --title "teste-\$i" &
    sleep 0.1
    i=\$((i + 1))
done
wait
INNEREOF
chmod +x "$INIT"

echo "Iniciando sessão aninhada River com $N janelas de teste..."
echo "Log: tail -f $LOG"
echo "Pressione Ctrl+C para encerrar."

trap "rm -f '$INIT'" EXIT
river -c "$INIT"
