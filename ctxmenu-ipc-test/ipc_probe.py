#!/usr/bin/env python3
"""
Valida o IPC do menu de contexto da MainDeck bar SEM precisar da barra real.

Ideia: a barra normalmente (a) escuta em $XDG_RUNTIME_DIR/maindeck-bar.sock para
receber a string "windows ..." do WM, e (b) envia comandos para
$XDG_RUNTIME_DIR/maindeck-wm.sock. Este probe faz os dois papéis:
  - faz bind em maindeck-bar.sock e captura as notificações "windows" do WM
    (de onde extrai um identifier real de janela);
  - envia maximize/minimize/restore <id> ao maindeck-wm.sock;
  - confere que a próxima notificação "windows" reflete o prefixo esperado
    (+ = maximizada, ! = minimizada, nenhum = normal).

Saída: linhas "RESULT: <nome> PASS|FAIL ...". Código de saída 0 se tudo PASS.
"""
import os, socket, sys, time, select

RT = os.environ["XDG_RUNTIME_DIR"]
BAR_SOCK = os.path.join(RT, "maindeck-bar.sock")
WM_SOCK  = os.path.join(RT, "maindeck-wm.sock")

results = []
def record(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"RESULT: {name} {'PASS' if ok else 'FAIL'} {detail}", flush=True)

# 1) bind no socket da barra para receber as notificações do WM
try:
    os.unlink(BAR_SOCK)
except FileNotFoundError:
    pass
bar = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
bar.bind(BAR_SOCK)
bar.setblocking(False)

wm = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)

def drain_windows(timeout=2.0):
    """Coleta a ÚLTIMA mensagem 'windows ...' recebida dentro de `timeout`s."""
    last = None
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([bar], [], [], max(0, end - time.time()))
        if not r:
            continue
        try:
            data = bar.recv(8192).decode("utf-8", "replace")
        except BlockingIOError:
            continue
        if data.startswith("windows"):
            last = data
    return last

def parse_ids(msg):
    """Retorna lista de (prefixo, id) a partir de 'windows [!|+]id ...'."""
    out = []
    for tok in msg.split()[1:]:
        if tok and tok[0] in "!+":
            out.append((tok[0], tok[1:]))
        else:
            out.append(("", tok))
    return out

def send_wm(verb, ident):
    wm.sendto(f"{verb} {ident}".encode(), WM_SOCK)

def prefix_of(msg, ident):
    for pfx, i in parse_ids(msg):
        if i == ident:
            return pfx
    return None  # janela sumiu

# 2) espera o WM publicar pelo menos uma janela viva
first = None
deadline = time.time() + 8.0
while time.time() < deadline:
    m = drain_windows(1.0)
    if m and len(parse_ids(m)) >= 1:
        first = m
        break

if not first:
    record("setup_windows_published", False, "(nenhuma janela publicada pelo WM)")
    bar.close(); wm.close()
    sys.exit(1)

ids = parse_ids(first)
record("setup_windows_published", True, f"(n={len(ids)} msg={first.strip()!r})")
# escolhe a primeira janela com prefixo vazio (normal) — alvo dos comandos
target = None
for pfx, i in ids:
    if pfx == "":
        target = i
        break
if target is None:
    target = ids[0][1]
print(f"INFO: target identifier = {target}", flush=True)

# Caso A: maximize → espera prefixo '+'
send_wm("maximize", target)
m = drain_windows(2.5)
record("maximize_sets_plus", m is not None and prefix_of(m, target) == "+",
       f"(prefix={prefix_of(m, target) if m else None})")

# Caso B: restore (estava maximizada) → volta a normal (sem prefixo)
send_wm("restore", target)
m = drain_windows(2.5)
record("restore_clears_plus", m is not None and prefix_of(m, target) == "",
       f"(prefix={prefix_of(m, target) if m else None})")

# Caso C: minimize → espera prefixo '!'
send_wm("minimize", target)
m = drain_windows(2.5)
record("minimize_sets_bang", m is not None and prefix_of(m, target) == "!",
       f"(prefix={prefix_of(m, target) if m else None})")

# Caso D: restore (estava minimizada) → volta a normal
send_wm("restore", target)
m = drain_windows(2.5)
record("restore_unminimizes", m is not None and prefix_of(m, target) == "",
       f"(prefix={prefix_of(m, target) if m else None})")

# Caso E: identifier inválido NÃO deve derrubar o WM — manda e confere que
# ainda recebemos 'windows' depois (WM vivo).
send_wm("maximize", "@@@invalido@@@")
m = drain_windows(2.5)
record("invalid_id_wm_alive", m is not None, "(WM continuou publicando windows)")

bar.close(); wm.close()
ok_all = all(ok for _, ok, _ in results)
print(f"SUMMARY: {sum(1 for _,o,_ in results if o)}/{len(results)} PASS", flush=True)
sys.exit(0 if ok_all else 2)
