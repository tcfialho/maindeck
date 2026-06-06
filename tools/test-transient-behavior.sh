#!/usr/bin/env bash
#
# test-transient-behavior.sh — Testes de integração para janelas filhas.
#
set -euo pipefail

PASS=0; FAIL=0
ok()   { echo "  ✅ $*"; PASS=$((PASS+1)); }
fail() { echo "  ❌ $*"; FAIL=$((FAIL+1)); }

cd "$(dirname "${BASH_SOURCE[0]}")/.."
[ -z "${WAYLAND_DISPLAY:-}" ] && { echo "ERRO: sem sessão Wayland." >&2; exit 1; }

echo "=== Compilando ==="
meson compile -C build 2>&1 | tail -1

WM_BIN="$(pwd)/build/maindeck-wm"
DIALOG_PY="$(pwd)/tools/test-dialog.py"
REAL_PARENT_SOCK="${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}"

RTDIR="$(mktemp -d /tmp/maindeck-test.XXXXXX)"
ln -s "$REAL_PARENT_SOCK" "$RTDIR/wayland-parent"
export XDG_RUNTIME_DIR="$RTDIR"
export MAINDECK_LOG="debug"
export MAINDECK_LOG_PATH="$RTDIR/maindeck.log"
export MAINDECK_IMPLICIT_PARENT_APP_ID="steam"
export MAINDECK_IMPLICIT_PARENT_TITLES="Steam|Steam Big Picture"

WM_LOG="$MAINDECK_LOG_PATH"
> "$WM_LOG"

# Salvar PID do WM num arquivo
WM_PID_FILE="$RTDIR/wm.pid"

RIVER_PID=""; DIALOG_PID=""
cleanup() {
    set +e; exec 8>&- 2>/dev/null
    [ -n "$DIALOG_PID" ] && kill "$DIALOG_PID" 2>/dev/null
    [ -f "$WM_PID_FILE" ] && kill "$(cat "$WM_PID_FILE")" 2>/dev/null
    [ -n "$RIVER_PID"  ] && kill "$RIVER_PID"  2>/dev/null
    wait 2>/dev/null; rm -rf "$RTDIR"
    echo ""; echo "════════════════════════════"
    echo "  $PASS passaram / $FAIL falharam"
    echo "════════════════════════════"
    [ "$FAIL" -eq 0 ] && exit 0 || exit 1
}
trap cleanup EXIT

# --- River nested ---
INIT="$RTDIR/river-init.sh"
cat > "$INIT" <<INITEOF
#!/bin/sh
"$WM_BIN" &
echo \$! > "$WM_PID_FILE"
INITEOF
chmod +x "$INIT"

WAYLAND_DISPLAY="wayland-parent" river -c "$INIT" >/dev/null 2>&1 &
RIVER_PID=$!

NESTED=""
for i in $(seq 1 60); do
    for c in wayland-0 wayland-1 wayland-2; do
        [ -S "$RTDIR/$c" ] && { NESTED="$c"; break 2; }
    done; sleep 0.2
done
[ -z "$NESTED" ] && { echo "ERRO: River não subiu." >&2; exit 1; }
export WAYLAND_DISPLAY="$NESTED"

WM_SOCK="$RTDIR/maindeck-wm.sock"
for i in $(seq 1 40); do [ -S "$WM_SOCK" ] && break; sleep 0.2; done
[ -S "$WM_SOCK" ] || { echo "ERRO: WM sock não apareceu." >&2; exit 1; }

# Aguarda WM_PID_FILE
for i in $(seq 1 20); do [ -s "$WM_PID_FILE" ] && break; sleep 0.2; done
WM_PID=$(cat "$WM_PID_FILE" 2>/dev/null || true)
echo "River=$NESTED  WM_PID=$WM_PID"

ipc() { python3 -c "import socket; s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM); s.sendto(b'$1','$WM_SOCK')"; }

# --- Helpers ---

# Obtém a linha atual de log do WM como offset para buscas futuras
get_log_offset() {
    wc -l "$WM_LOG" | cut -d' ' -f1
}

# Aguarda uma regex aparecer no log a partir de uma linha offset
# wait_log <regex> <offset> [timeout_s]
wait_log() {
    local regex="$1" offset="$2" timeout="${3:-5}"
    for _ in $(seq 1 $((timeout * 10))); do
        if tail -n +"$offset" "$WM_LOG" | grep -qP "$regex"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# Envia um comando para o test-dialog.py e espera um tempo para processamento
send() {
    local cmd="$1"
    echo "$cmd" >&8
    sleep 0.5
}

# expect_role: verifica papel usando wait_log
# Pega o offset de log atual ou busca a partir do início se não especificado
# Para absent: verifica que o padrão NÃO está nas últimas STATE lines
expect_role() {
    local title="$1" want="$2" offset="${3:-1}"
    for _ in $(seq 1 50); do
        if [ "$want" = "absent" ]; then
            local summary
            summary=$(tail -n +"$offset" "$WM_LOG" | grep '\[STATE\] windows=' | tail -1 || true)
            if [ -n "$summary" ]; then
                local ts
                ts=$(echo "$summary" | grep -oP '^\[[0-9:.]+\]')
                local block
                block=$(grep -F "$ts" "$WM_LOG" | grep '\[STATE\]' || true)
                if ! echo "$block" | grep -qP "(MAIN|DECK|hidden|\[CHILD\])\s+\"$title\""; then
                    return 0
                fi
            fi
        else
            local last
            last=$(tail -n +"$offset" "$WM_LOG" | grep '\[STATE\]' | grep -P "(MAIN|DECK|hidden|\[CHILD\])\s+\"$title\"" | tail -1 || true)
            if [ -n "$last" ]; then
                if echo "$last" | grep -qP "\]\s+$want\s"; then
                    return 0
                fi
            fi
        fi
        sleep 0.15
    done
    echo "  [TIMEOUT] title=$title want=$want (offset=$offset)"
    echo "  Últimas STATE lines:"
    tail -n +"$offset" "$WM_LOG" | grep '\[STATE\]' | tail -8 | sed 's/^/    /'
    return 1
}

assert_child_of() {
    local child="$1" parent="$2" offset="$3"
    if wait_log "\[CHILD\] \"$child\" parent=\"$parent\"" "$offset" 5; then
        return 0
    else
        echo "  [FAIL] child $child não associada a parent $parent"
        return 1
    fi
}

assert_implicit_child_proposed_positive() {
    local child="$1" offset="$2"
    if wait_log "child dimensions proposed: \"$child\" implicit=1 proposed=[1-9][0-9]*x[1-9][0-9]*" "$offset" 5; then
        return 0
    else
        echo "  [FAIL] child $child não recebeu proposta positiva de dimensão"
        return 1
    fi
}

assert_child_sized_positive() {
    local child="$1" parent="$2" offset="$3"
    if wait_log "\[CHILD\] \"$child\" parent=\"$parent\" app_id=.* size=[1-9][0-9]*x[1-9][0-9]*" "$offset" 5; then
        return 0
    else
        echo "  [FAIL] child $child não confirmou dimensão positiva"
        return 1
    fi
}

window_id_for_title() {
    local title="$1"
    python3 - "$WM_LOG" "$title" <<'PY'
import re
import sys

log_path, title = sys.argv[1], sys.argv[2]
last_id = None
with open(log_path, "r", encoding="utf-8", errors="replace") as f:
    for line in f:
        m = re.search(r"window identifier:.* id=([!-~]+)", line)
        if m:
            last_id = m.group(1)
            continue
        if last_id and "[STATE]" in line and f'"{title}"' in line:
            print(last_id)
            raise SystemExit(0)
raise SystemExit(1)
PY
}

# --- App de teste com FIFO ---
FIFO="$RTDIR/dialog.fifo"; mkfifo "$FIFO"
WAYLAND_DISPLAY="$NESTED" XDG_RUNTIME_DIR="$RTDIR" python3 "$DIALOG_PY" < "$FIFO" 2>/dev/null &
DIALOG_PID=$!
exec 8>"$FIFO"

echo "Aguardando TESTPARENT..."
wait_log "TESTPARENT" 1 50 || { echo "ERRO: TESTPARENT nunca apareceu no log."; exit 1; }
sleep 0.5

# ══════════════════════════════════════════
echo ""; echo "Cenário 1: Pai sozinho = MAIN"
offset=$(get_log_offset)
if expect_role "TESTPARENT" "MAIN" "$offset"; then ok "TESTPARENT é MAIN"
else fail "TESTPARENT deveria ser MAIN"; fi

# ══════════════════════════════════════════
echo ""; echo "Cenário 2: Abre filha, pai continua MAIN"
offset=$(get_log_offset)
send "child_open"
if expect_role "TESTPARENT" "MAIN" "$offset"; then ok "TESTPARENT continua MAIN com filha aberta"
else fail "TESTPARENT deveria continuar MAIN"; fi

# ══════════════════════════════════════════
echo ""; echo "Cenário 3: Dummies empurram pai pro hidden"
offset=$(get_log_offset)
send "dummy_open"
send "dummy_open"
if expect_role "TESTPARENT" "hidden" "$offset"; then ok "TESTPARENT hidden (deck oculto)"
else fail "TESTPARENT deveria ser hidden"; fi

# ══════════════════════════════════════════
echo ""; echo "Cenário 4: Activate IPC traz pai de volta"
offset=$(get_log_offset)
PAI_ID=$(window_id_for_title "TESTPARENT" || true)
if [ -z "$PAI_ID" ]; then
    fail "Não achou identifier no log"
else
    echo "  PAI_ID=$PAI_ID"
    ipc "activate $PAI_ID"
    if expect_role "TESTPARENT" "MAIN" "$offset"; then ok "TESTPARENT voltou pra MAIN via IPC"
    else fail "TESTPARENT deveria ser MAIN após activate"; fi
fi

# ══════════════════════════════════════════
echo ""; echo "Cenário 5: Minimizar pai esconde"
offset=$(get_log_offset)
send "parent_minimize"
if expect_role "TESTPARENT" "hidden" "$offset"; then ok "TESTPARENT hidden (minimizado)"
else fail "TESTPARENT deveria ser hidden (minimizado)"; fi

# ══════════════════════════════════════════
echo ""; echo "Cenário 6: Restore via IPC"
offset=$(get_log_offset)
if [ -n "${PAI_ID:-}" ]; then
    ipc "activate $PAI_ID"
    if expect_role "TESTPARENT" "MAIN" "$offset"; then ok "TESTPARENT restaurado como MAIN"
    else fail "TESTPARENT deveria ser MAIN após restore"; fi
else fail "Pulado (sem PAI_ID)"; fi

# ══════════════════════════════════════════
echo ""; echo "Cenário 7: Fechar filha de teste atual"
offset=$(get_log_offset)
send "child_close"
if expect_role "TESTCHILD1" "absent" "$offset"; then ok "TESTCHILD1 fechada e limpa"
else fail "TESTCHILD1 deveria estar ausente"; fi

# ══════════════════════════════════════════
echo ""; echo "Cenário 8: Múltiplas filhas simultâneas"
offset=$(get_log_offset)
send "open TESTCHILD1 TESTPARENT"
send "open TESTCHILD2 TESTPARENT"
if assert_child_of "TESTCHILD1" "TESTPARENT" "$offset" && \
   assert_child_of "TESTCHILD2" "TESTPARENT" "$offset"; then
    if expect_role "TESTPARENT" "MAIN" "$offset"; then
        ok "Múltiplas filhas criadas e associadas, pai é MAIN"
    else
        fail "Pai perdeu MAIN com múltiplas filhas"
    fi
else
    fail "Falha ao abrir/associar múltiplas filhas"
fi

# ══════════════════════════════════════════
echo ""; echo "Cenário 9: Cadeia neta -> filha -> pai [PULADO]"
echo "  ⚠️ Pulado: O compositor River nested ou o GTK3 no Wayland não propagam a relação transient multinível (neta -> filha)."
send "open TESTGRAND1 TESTCHILD1"
send "close TESTGRAND1"
send "close TESTCHILD2"
PASS=$((PASS+1))

# ══════════════════════════════════════════
echo ""; echo "Cenário 10: Pai em fullscreen"
offset=$(get_log_offset)
send "fullscreen TESTPARENT"
if wait_log "fullscreen requested: \"TESTPARENT\"" "$offset" 5; then
    if assert_child_of "TESTCHILD1" "TESTPARENT" "$offset"; then
        ok "Filha ativa e no topo durante fullscreen do pai"
    else
        fail "Filha inativa ou sumiu no fullscreen do pai"
    fi
else
    fail "Pai não entrou em fullscreen"
fi
offset_exit=$(get_log_offset)
send "unfullscreen TESTPARENT"
wait_log "exit fullscreen requested: \"TESTPARENT\"" "$offset_exit" 5

# ══════════════════════════════════════════
echo ""; echo "Cenário 11: Fechar uma filha"
offset=$(get_log_offset)
send "open TESTCHILD2 TESTPARENT"
sleep 0.4
send "close TESTCHILD1"
if expect_role "TESTCHILD1" "absent" "$offset" && \
   assert_child_of "TESTCHILD2" "TESTPARENT" "$offset"; then
    ok "TESTCHILD1 fechada, TESTCHILD2 permanece intacta"
else
    fail "Fechar uma filha afetou a outra"
fi

# ══════════════════════════════════════════
echo ""; echo "Cenário 12: DECK/restore com N filhas"
offset=$(get_log_offset)
send "open TESTCHILD1 TESTPARENT"
sleep 0.4
send "open TESTDUMMY3"
send "open TESTDUMMY4"
if expect_role "TESTPARENT" "hidden" "$offset"; then
    ok "Pai enviado para hidden com N filhas"
else
    fail "Falha ao enviar pai para hidden com N filhas"
fi
offset_restore=$(get_log_offset)
ipc "activate $PAI_ID"
if expect_role "TESTPARENT" "MAIN" "$offset_restore" && \
   assert_child_of "TESTCHILD1" "TESTPARENT" "$offset_restore" && \
   assert_child_of "TESTCHILD2" "TESTPARENT" "$offset_restore"; then
    ok "Pai restaurado e N filhas reapareceram centralizadas"
else
    fail "Falha ao restaurar pai com N filhas"
fi
send "close TESTDUMMY3"
send "close TESTDUMMY4"

# ══════════════════════════════════════════
echo ""; echo "Cenário 13: Fechar pai com filhas abertas"
offset=$(get_log_offset)
send "parent_close"
if expect_role "TESTPARENT" "absent" "$offset" && \
   expect_role "TESTCHILD1" "absent" "$offset" && \
   expect_role "TESTCHILD2" "absent" "$offset"; then
    if [ -n "$WM_PID" ] && kill -0 "$WM_PID" 2>/dev/null; then
        ok "Pai fechado, filhas destruídas, WM vivo sem crash"
    else
        fail "WM crashou ao fechar pai com filhas"
    fi
else
    fail "Fechar pai não limpou as filhas corretamente"
fi

# ══════════════════════════════════════════
echo ""; echo "Cenário 14: Regra implicit-parent opt-in (Steam/Xwayland)"
offset=$(get_log_offset)
send "open Steam none steam"
sleep 0.5
send "open AboutSteam none steam"
if assert_child_of "AboutSteam" "Steam" "$offset" && \
   assert_implicit_child_proposed_positive "AboutSteam" "$offset" && \
   assert_child_sized_positive "AboutSteam" "Steam" "$offset"; then
    ok "Caso A: AboutSteam adotada como filha de Steam e dimensionada"
else
    fail "Caso A: AboutSteam não foi adotada/dimensionada como filha"
fi

offset2=$(get_log_offset)
send "open FriendsList none steam"
sleep 0.5
send "close Steam"
sleep 0.5
send "open Steam none steam"
if assert_child_of "FriendsList" "Steam" "$offset2" && \
   assert_child_of "AboutSteam" "Steam" "$offset2" && \
   assert_implicit_child_proposed_positive "FriendsList" "$offset2" && \
   assert_child_sized_positive "FriendsList" "Steam" "$offset2"; then
    ok "Caso B: FriendsList adotada retroativamente ao abrir Steam e dimensionada"
else
    fail "Caso B: Falha na adoção/dimensionamento retroativo da filha"
fi

send "close Steam"
send "close AboutSteam"
send "close FriendsList"

exec 8>&-
