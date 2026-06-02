# Fix: clique na taskbar (waybar) não dá foco / não traz janela oculta

## Estado atual do repo
- HEAD limpo. Há DBG temporário instrumentando o problema (REMOVER no fim):
  - `maindeck-wm.c` em `seat_handle_window_interaction` e em `seat_manage` (linhas com `DBG`).
  - `maindeck-proxy.c` em `relay_c2s` (`DBG c2s on real handle`).
- Há um `git stash@{0}` "wip taskbar activation ipc fix" — NÃO aplicar direto (ver §5).
- Binários instalados em `~/.local/bin/`. Build: `ninja -C build && ninja -C build install`.
  Roda só após RELOG do River (proxy não faz hot-swap).
- Log do WM: `~/.local/state/river/session.log` (stderr) e `~/.local/state/maindeck/maindeck.log`.
  NÃO limpar o session.log (é o registro durável). Marcar tempo e ler trecho novo.

## 1. CAUSA RAIZ (provada com log, não suposição)
Clique na taskbar `wlr/taskbar` → waybar manda `zwlr_foreign_toplevel_handle_v1.activate`
(opcode 4) no handle → proxy repassa pro River → **River IGNORA**. Não existe
`activate_requested` no protocolo `river_window_v1` (confirmado: só tem
maximize/minimize/fullscreen_requested). Então o foco nunca acontece.

Prova no log: 3 cliques geraram `[proxy] DBG c2s on real handle: obj=... opcode=4 drop=0`
e ZERO mudança de STATE/foco/manage no WM.

NÃO é `window_interaction`: esse evento só dispara em clique DIRETO na janela visível,
não no clique da taskbar. (Tentativas anteriores via window_interaction falharam por isso.)

## 2. POR QUE as abordagens fáceis não servem
- **window_interaction / idx>=2 no seat_manage**: o evento não chega no clique da taskbar. Morto.
- **Ordem de criação (zwlr handle ↔ river_window por índice)**: FRÁGIL. O WM cria
  river_window pra TODA janela (sem filtro); o foreign-toplevel lista só toplevels
  normais (exclui floating/xwayland/popups em casos). Uma divergência → todos os
  índices desalinham → TODO clique foca errado, pra sempre, em silêncio. Descartado.
- **app_id+title (B)**: NÃO serve. Usuário abre 10 Thunars (app_id E título idênticos)
  → sempre erra. Descartado por requisito real.

## 3. A SOLUÇÃO CORRETA (ext-foreign-toplevel-list + IPC socket)
O `ext_foreign_toplevel_handle_v1` tem `identifier` ÚNICO por janela. O River:
- cria ext handle E zwlr handle JUNTOS, mesma ordem (ext antes), por janela
  (`references/river/river/Window.zig:422-440`).
- destrói os dois JUNTOS, mesma ordem (`Window.zig:1182-1190`).
- `river_window.identifier` (que o WM recebe) = `ext_handle.identifier` (`Window.zig:455`).
→ INVARIANTE garantido pelo River: ext e zwlr são lockstep (both-or-neither, mesma ordem,
  create e destroy). É isso que torna o mapeamento por índice SEGURO aqui.

River expõe `ext_foreign_toplevel_list_v1` v1 (confirmado: `wayland-info` no display real
`wayland-1` → name 26). waybar usa só zwlr (name 25), NÃO ext.

### Fluxo a implementar
1. **Proxy** abre conexão DEDICADA ao River real e binda `ext_foreign_toplevel_list_v1`.
   Mantém lista `ext_toplevels` em ordem de chegada (cada um traz `identifier`).
2. **Proxy** já rastreia `real_handle_ids[]` (zwlr handles em ordem de chegada, Direction B).
   Mapeia: índice do zwlr handle ativado → mesmo índice na lista ext → `identifier`.
   (Seguro por causa do invariante lockstep acima.)
3. **Proxy** intercepta `activate` (opcode 4) no zwlr handle, resolve o `identifier`,
   manda pro WM via socket Unix DGRAM (`$XDG_RUNTIME_DIR/maindeck-wm.sock`),
   mensagem `"activate <identifier>"`. NÃO repassa o activate pro River (ele ignora mesmo).
4. **WM** já recebe `river_window.identifier` no handler `window_handle_identifier`
   (hoje VAZIO, linha ~611). GUARDAR esse identifier num campo novo `char identifier[33]`
   no `struct Window`.
5. **WM** escuta o socket DGRAM no event loop (adicionar fd ao `poll` junto com
   wl_display fd). Ao receber `"activate <id>"`: achar a Window com aquele identifier,
   aplicar política (§4 abaixo).

## 4. POLÍTICA de foco (decisão do usuário)
- Janela VISÍVEL (idx 0=MAIN ou 1=DECK): só vira ALVO (foco). NÃO mover.
- Janela OCULTA (idx >= 2): promover pra MAIN (`move_first` + `target_index=0`).
- Sem match (id desconhecido): no-op silencioso (janela sem foreign-toplevel não está
  na taskbar mesmo).
- Mudança de estado deve rodar no manage cycle. Pra acordar o WM após receber o socket,
  pode ser necessário `river_window_manager_v1_manage_dirty()` ou aplicar no próximo
  manage. VERIFICAR: o WM precisa forçar um manage cycle pra o foco/layout aplicar.

## 5. ARMADILHAS (o que quebrou antes — NÃO repetir)
- **O stash quebrou a waybar** por `wl_display_connect(NULL)`: isso relê `WAYLAND_DISPLAY`
  que na sessão River é `maindeck-0` (o PRÓPRIO proxy) → conecta em si mesmo + roundtrips
  bloqueantes no startup do worker → waybar trava. 
  → CORREÇÃO: conectar EXPLÍCITO no socket real do River. Reusar o mesmo path que
    `connect_to_river()` (maindeck-proxy.c:1321) já usa com sucesso por cliente —
    verificar qual socket ele resolve no ambiente do proxy (NÃO o env da sessão).
    NUNCA usar `wl_display_connect(NULL)`.
- **O stash mapeava por índice** (`ext_identifier_by_index_locked: if(i==index)`) —
  isso está CERTO dado o invariante lockstep, mas só funciona se a lista ext e a lista
  zwlr forem mantidas em sincronia no ADD e no REMOVE. Se fechar um handle e remover de
  uma lista e não da outra (ou em tempos diferentes), os índices desalinham → foco errado
  silencioso. → ENGENHARIA CRÍTICA: add e remove devem mover as DUAS listas juntas.
- O socket DGRAM NÃO foi o que quebrou a waybar (foi o self-connect). Pode usar.
- O stash mantinha lista paralela `ext_toplevels` — isso é NECESSÁRIO e OK (é a fonte da
  unicidade). O usuário aceitou.

## 6. APROVEITAR do stash (revisar, não aplicar cego)
`git stash show -p stash@{0}` tem ~325 linhas já escritas: estrutura ext listener,
`send_wm_activate`, `handle_taskbar_activate`, ipc_init/drain no WM, campo identifier.
A LÓGICA está quase certa. Os erros a corrigir ao reaproveitar:
  (a) trocar `wl_display_connect(NULL)` por conexão explícita ao River real (§5).
  (b) garantir sincronia ext-list ↔ zwlr-list no remove (§5).
  (c) garantir que o WM force manage cycle após receber o activate (§4).
meson.build do stash adiciona o protocolo:
  `/usr/share/wayland-protocols/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml`
  (presente no sistema, confirmado).

## 7. TESTE obrigatório (provar antes de declarar pronto — exigência do usuário)
Instrumentar cada elo com plog/LOG_EVENT (proxy vê activate → resolve id → manda socket →
WM recebe → acha janela → foca). Após relog:
1. Abrir 10 Thunars. Clicar no 5º na taskbar → o 5º (aquele exato) recebe foco. ← prova unicidade
2. Fechar um do meio, abrir outro, clicar em vários → todos certos. ← prova sincronia no remove
3. Clicar em janela visível não-focada → só foco. Clicar em oculta → vai pra MAIN.
NÃO declarar funcionando sem o teste #1 e #2 passarem (são os que pegam os 2 bugs estruturais).

## 8. Limpeza final
Remover todo o DBG temporário (maindeck-wm.c seat_handle_window_interaction e seat_manage;
maindeck-proxy.c relay_c2s "DBG c2s on real handle"). Commit separado.
