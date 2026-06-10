# Plano de otimização — maindeck (wm + bar + menu)

**Data:** 2026-06-10 · **Base:** `~/Documents/poc/maindeck-wm` @ `44fe6d8` + diff em fluxo
**Fonte:** leitura direta dos hot paths (Claude) + varredura agy verificada item a item.

## ⚠️ Coordenação — trabalho em fluxo AGORA

Há uma sessão agy paralela (desde 10:25 de hoje) editando `types.h`, `wm-layout.c/h`,
`wm-handlers.c`, `wm-input.c` — feature `window_is_ignored` (janelas-fantasma 1×1), com
review Opus em andamento. **Itens P1.3, P0.4 e P2 tocam esses arquivos: aplicar só depois
desse diff estabilizar (commit), e rebasear a análise.** Itens da barra/menu/ícones
(P0.1, P0.2, P0.3, P1.1, P3.*) não colidem — podem começar já.

## Diagnóstico geral

A arquitetura é saudável: tudo event-driven (poll, sem busy-wait), redraw coalescido por
dirty-flag (1 render por lote de eventos), hover só redesenha em transição, WM com dedup
por assinatura FNV do estado, gradientes/superfícies cairo cacheados, double-buffer shm.
**Os ganhos reais não estão em micro-otimização de CPU — estão em (A) I/O síncrono
escondido em caminhos interativos (ícones, D-Bus, pkill), (B) corrupção/UAF que limita
refactors, e (C) O(n²) estrutural no WM que é barato de remover e elimina uma classe de
bug da assinatura.**

---

## P0 — Latência interativa (impacto percebido pelo usuário)

### P0.1 — Ícones: tirar a varredura recursiva de FS do caminho de render 🔥 (o pior do projeto)
- **Problema:** `bar_icon_get()` (`bar-icons.c:287`) tem cache fixo de **64 slots sem
  eviction** (`bar-icons.c:17,363`). O menu chama `bar_icon_get` por item visível **a cada
  render/tecla** (`maindeck-menu.c:717`). Com mais de 64 ícones distintos (menu tem
  centenas de apps; rolagem passa por todos), o 65º nome em diante **nunca cacheia** e cada
  miss re-executa `find_icon_path` (~200 `access()`) e, no fim, `scan_dir_recursive()` em
  `/usr/share/icons`, `/usr/local/share/icons` e flatpak **inteiros** (`bar-icons.c:188-192`,
  `lstat` por inode, dezenas de milhares) — **por item, por tecla digitada, na thread de
  render**. A barra usa o mesmo resolver (1× por app_id novo; `bar-taskbar.c:65`, tray
  `bar-tray.c:138`).
- **Fix (em camadas, na ordem):**
  1. Cache dinâmico sem limite duro: hash table (FNV + open addressing, crescimento ×2)
     em vez do array de 64; miss (NULL) cacheado para sempre — já é a intenção atual,
     só não cabe no array.
  2. No menu, memoizar a `cairo_surface_t*` resolvida no próprio `struct App` (campo
     `icon_surface`), preenchida lazy no primeiro render do item — remove até o lookup
     de hash do caminho por frame.
  3. `scan_dir_recursive` nunca mais que 1× por nome (já coberto pelo NULL-cache do
     item 1) e com profundidade limitada; opcional: só executá-lo fora do primeiro
     frame (pré-resolver os MAX_ITEMS visíveis após `filter_apps`).
- **Impacto:** ALTO — elimina travadas de digitação no menu e stutter da barra ao abrir
  app com ícone ausente. · **Esforço:** M (1 arquivo + 1 campo no menu).
- **Verificação:** `strace -c -e trace=lstat,access,openat ./maindeck-menu` antes/depois
  digitando 5 letras com >64 apps; contagem de syscalls deve cair ordens de magnitude.

### P0.2 — Tray: D-Bus assíncrono (a barra congela até 2s hoje)
- **Problema:** chamadas bloqueantes no event loop da barra:
  `load_item_props` → `send_with_reply_and_block(…, 1000ms)` (`bar-tray.c:90`) a cada item
  SNI registrado; `bar_tray_open_menu` → `GetLayout` bloqueante de **2000ms**
  (`bar-tray.c:981`) no clique. App SNI lento/zumbi = barra inteira congelada (sem hover,
  sem relógio, sem cliques).
- **Fix:** `dbus_connection_send_with_reply` + `dbus_pending_call_set_notify` (o fd do
  D-Bus já está no poll — `bar-tray.c:547`); renderizar item com placeholder até a reply
  chegar; menu do tray abre quando o layout chegar (ou spinner curto).
- **Impacto:** ALTO em robustez percebida (pior caso some). · **Esforço:** M.
- **Verificação:** criar SNI de teste que dorme 5s no GetAll/GetLayout; barra deve
  continuar respondendo a hover/clique.

### P0.3 — `close_launcher`: fork+exec de `pkill` a cada clique em janela
- **Problema:** `seat_handle_window_interaction` (`wm-input.c:340-344`) chama
  `close_launcher()` que faz `fork`+`execlp("pkill", "-x", "maindeck-menu")`
  (`wm-input.c:31-35`) — **todo clique em qualquer janela** dispara um processo que varre
  `/proc` inteiro, mesmo com menu fechado (caso ~100% do tempo).
- **Fix (escolher 1):**
  a) O menu já tem socket de instância única (`maindeck-menu.c:478-527`, comando `"C"`):
     o WM manda 1 datagrama UDP-unix (≈3 linhas, sem fork) — **preferido**, mecanismo já existe;
  b) ou guardar o PID do `spawn_command("maindeck-menu")` e `kill(pid, SIGTERM)` direto.
- **Impacto:** MÉDIO (latência/ruído por clique; hoje é o único fork em hot path). · **Esforço:** S.
- **Verificação:** `MAINDECK_LOG=debug` + clicar em janelas: nenhum processo pkill em
  `ps`; menu ainda fecha ao clicar fora.

### P0.4 — Hold (Super+Tab/←/→) provavelmente só age no próximo evento — VERIFICAR antes
- **Hipótese (código, não medido):** quando o hold dispara por timeout do poll
  (`wm-ipc.c:206-209` → `process_hold_timers`, `wm-input.c:171-190`), `seat_action` só
  altera estado local (swap/promote/send mexem na lista; `focus_dirty=true`). **Nenhum
  request Wayland é emitido e não há `manage_dirty` em wm-input.c** (grep confirma) — o
  River não tem motivo para iniciar novo ciclo de manage; o efeito visual ficaria
  represado até o próximo evento (ex.: soltar a tecla).
- **Verificar primeiro (obrigatório):** `MAINDECK_LOG=debug`, segurar Super+Tab >360ms
  SEM soltar: o swap aparece na tela no instante do "hold fired" ou só ao soltar?
  Comparar timestamps `[HH:MM:SS.mmm]` de `hold fired` → próximo `windows=` no log.
- **Fix se confirmado:** após qualquer `hold_fired` em `process_hold_timers` chamado do
  main loop, emitir `river_window_manager_v1_manage_dirty()` (+ flush no topo do loop já
  existente). 1-2 linhas.
- **Impacto:** ALTO se confirmado (360ms+tempo-de-soltar de latência em 3 atalhos centrais).
  · **Esforço:** S. · **⚠️ toca wm-input.c (em fluxo — esperar diff estabilizar).**

---

## P1 — Corretude que destrava otimização

### P1.1 — Taskbar da barra: user_data stale após `memmove` (corrupção real)
- **Problema:** `mgr_toplevel` registra listener com `data = &bar->toplevels[idx]`
  (`bar-taskbar.c:183`). `tl_closed` compacta o array com `memmove`
  (`bar-taskbar.c:117-123`) — os listeners das janelas seguintes continuam apontando para
  os slots antigos. `tl_closed` se defende buscando por handle (comentário em
  `bar-taskbar.c:111` admite o problema), **mas `tl_title`/`tl_app_id`/`tl_state`/
  `tl_parent` usam `data` direto** → após fechar uma janela, o título/estado de uma janela
  escreve no slot de outra. O pareamento zwlr↔ext por lockstep de chegada
  (`bar-taskbar.c:176-179`, `ext_identifier:217`) tem a mesma fragilidade, e `ext_closed`
  (`bar-taskbar.c:240-263`) deixa `tl->ext_handle` dangling (hoje não desreferenciado, mas
  é mina armada).
- **Fix:** slots estáveis — `struct BarToplevel *toplevels[BAR_MAX_TOPLEVELS]` com structs
  `calloc`'d (free no closed, sem memmove de conteúdo; compactar só o array de ponteiros).
  `data` passa a ser estável por construção. Aproveitar e zerar `tl->ext_handle` no
  `ext_closed`. (O pareamento robusto por identifier já tem plano próprio:
  `otimizacao-jogos-task/future-matching-plan.md` — não duplicar aqui.)
- **Impacto:** ALTO (corrige corrupção visível: título errado no botão após fechar janela;
  pré-requisito para qualquer cache por-toplevel de P3.2). · **Esforço:** M (1 arquivo,
  mudança mecânica de `tl[i].x` → `tl[i]->x` + render/input que indexam).
- **Verificação:** abrir 4 janelas, fechar a 1ª, mudar título das demais (ex.: trocar aba
  no navegador) — títulos devem atualizar no botão certo.

### P1.2 — `seat->interacted`/`hovered` dangling após destruir janela (UAF)
- **Problema:** `window_destroy_closed` limpa `seat->focused` (`wm-handlers.c:460-463`)
  mas **não** `seat->interacted`/`seat->hovered`. Em `wm_handle_manage_start` os destroys
  rodam **antes** de `seat_manage` (`wm-handlers.c:626-639`), que então desreferencia
  `seat->interacted` (`wm-input.c:512-525`) — janela clicada que fecha no mesmo lote =
  use-after-free.
- **Fix:** no mesmo loop que limpa `focused`, limpar `interacted` e `hovered`. `hovered`
  aliás é escrito (`wm-input.c:332,337`) e nunca lido — remover o campo ou começar a usá-lo.
- **Impacto:** MÉDIO (crash raro mas real; sujeira de valgrind). · **Esforço:** S.
  · **⚠️ wm-handlers.c em fluxo.**

### P1.3 — `compute_layout_signature` não cobre o que o layout passou a ler
- **Problema:** a dedup por assinatura (`wm-handlers.c:509-552`) só mixa `width/height`
  de janelas `parent!=NULL || floating`. O diff em fluxo faz o layout depender de
  `window_is_ignored(w)` (= `width/height ≤1` + `seen_real_dimensions`) para janelas
  **root tiladas** — que não entram na assinatura. Quando a janela-fantasma ganha
  dimensão real, `manage_dirty` é chamado (`wm-handlers.c:176-179`) mas o manage recalcula
  a MESMA assinatura e **pula o relayout** (`wm-handlers.c:661-678`). A janela pode ficar
  fora do tiling até outro evento mexer na sig.
- **Fix:** mixar `window_is_ignored(w)` (1 bit) por janela na assinatura — ou width/height
  de todas. Regra permanente a documentar no código: *todo estado lido por
  `window_manage_layout`/`window_render_layout` TEM que entrar na assinatura* (P2 resolve
  isso por construção).
- **Impacto:** ALTO para a feature em fluxo (sem isso ela falha intermitente). ·
  **Esforço:** S. · **⚠️ é correção DO diff em fluxo — apontar para a outra sessão/review
  em vez de aplicar por fora.**

---

## P2 — Algoritmo do WM: visão de layout em passada única (O(n²) → O(n))

- **Problema (padrão difuso, confirmado também pelo agy):** as primitivas
  `window_at`/`window_index`/`visible_window_count`/`target_window` são O(n) e são chamadas
  **por janela** dentro dos ciclos de manage/render → O(n²) por ciclo:
  - `window_apply_borders` → `target_window()` por janela (`wm-layout.c:28`);
  - `layout_box_for_index` → `visible_window_count()` por janela (`wm-layout.c:155`);
  - `window_render_layout` (children) → `window_index(root)` + `window_is_really_visible`
    → `window_index` de novo (`wm-layout.c:594,657`);
  - `focus_target_on_seats` → `window_is_really_visible` por child (`wm-layout.c:303-310`);
  - `wm_handle_render_start` chama `target_window()` 2× (`wm-handlers.c:714-716`).
- **Custo real hoje:** baixo (n<20 janelas; ciclos só em mudança de estado — a dedup por
  assinatura já evita ciclos ociosos). **O valor é estrutural:** elimina a classe de bug
  P1.3 e simplifica os 5 pontos acima.
- **Fix:** materializar no início de cada ciclo uma `struct LayoutView` (array pequeno na
  stack/static): roots tiladas visíveis em ordem (`{win, index}`), `visible_count`,
  `target` resolvido 1×, e por-janela o `Box` já calculado. `window_manage_layout`/
  `window_render_layout`/`window_apply_borders` recebem a view (ou `is_target` e `box`
  como parâmetros). A assinatura passa a ser computada **da própria view** → impossível
  o layout ler algo fora dela.
- **Impacto:** MÉDIO (manutenibilidade/corretude > CPU). · **Esforço:** M-L (toca
  wm-layout.c e wm-handlers.c inteiros). · **⚠️ esperar diff em fluxo; idealmente é o
  refactor que ABSORVE `window_is_ignored` de forma limpa.**

---

## P3 — Render e startup (polimento, ganhos menores)

### P3.1 — Damage parcial na barra
- `bar_surface_commit` sempre damageia o buffer inteiro (`bar-surface.c:418`); cada
  transição de hover/tick de relógio re-blita 1920×32. Com double-buffer, damage parcial
  exige acumular os rects dos **2 últimos frames** (buffer age). Alternativa mais simples
  com quase todo o ganho: damageiar a união {seção que mudou} (taskbar | status | tray)
  marcada por quem setou dirty. **Impacto:** BAIXO-MÉDIO (barra é 0,25% da tela; render já
  é só em transição). · **Esforço:** M. Só fazer depois de P0/P1.

### P3.2 — Cache de PangoLayout por título na taskbar
- `draw_taskbar` re-shapeia (Pango) cada título a cada render (`bar-render.c:322`);
  shaping é o item mais caro do frame da barra. Cachear `PangoLayout` por toplevel,
  invalidando em `tl_title` — **depende de P1.1** (slots estáveis). Idem relógio
  (`bar-render.c:511-525`, re-shapeia por render; só muda 1×/min). **Impacto:** BAIXO
  (frame da barra ~centenas de µs). · **Esforço:** S-M.

### P3.3 — Menu: startup e parsing
- a) `realloc(napps+1)` por app (`maindeck-menu.c:287`) → capacidade ×2 (S, trivial);
- b) `trim_and_dup` de key E value por linha de .desktop (`maindeck-menu.c:218-219`)
  → parse in-place no buffer da linha, `strdup` só do que for guardado (S);
- c) dedup por basename é O(n²) total (`maindeck-menu.c:279-284`) — n≈centenas, ok manter;
- d) **decisão de design (a maior alavanca do menu):** o menu é re-executado do zero a
  cada Super (`spawn_command("maindeck-menu")`, `wm-input.c:406`) → re-parse de centenas
  de .desktop + roundtrips Wayland a cada abertura. Opções: cache serializado em
  `~/.cache/maindeck/apps.cache` validado por mtime dos diretórios (M, mantém processo
  efêmero — **recomendado**) ou daemon residente com show/hide (L, muda modelo de foco).
- e) pré-lowercase de `name` para o `strcasecmp` do qsort (`maindeck-menu.c:357,368`) —
  só se sobrar vontade; n pequeno, BAIXO.

### P3.4 — Status: enum em vez de strcmp por render
- `draw_status` compara strings por módulo por frame (`bar-render.c:466,487,499,511`).
  Converter para enum no `bar_config_load`. **Impacto:** micro. · **Esforço:** S.

---

## Achados colaterais (não-perf; corrigir oportunisticamente)

| # | O quê | Onde |
|---|-------|------|
| C1 | Ícone de volume desenha **hardcoded** `vol=50, muted=false` — nunca reflete volume/mute reais | `bar-render.c:508` |
| C2 | `config.icon_theme` é parseado e **nunca usado** — `find_icon_path` tem temas hardcoded `{hicolor, breeze, breeze-dark, Papirus}` | `bar-config.c:105` vs `bar-icons.c:118` |
| C3 | Tray não escuta `NewIcon`/`NewTitle` — ícone/título do item nunca atualizam após registro | `bar-tray.c:196+` (filter sem esses members) |
| C4 | `make_layout` é dead code | `bar-render.c:113` |
| C5 | `seat->hovered` escrito e nunca lido | `wm-input.c:332,337` |
| C6 | Menu: `wl_display_roundtrip` dentro de `render()` quando 2 buffers ocupados (stall síncrono raro) | `maindeck-menu.c:629` |

## Anti-itens — medido/avaliado e NÃO vale mexer

- `hit_test` linear (`bar-input.c:15`): ≤128 comparações de int por motion — ótimo como está.
- `wl_list` do WM: n<20; trocar por array/hash não paga (P2 já remove os O(n²) de uso).
- `compute_poll_timeout` O(bindings) por wakeup (`wm-input.c:150`): ~20 itens, nada.
- Fuzzy match + qsort do menu por tecla: n≈centenas, custo µs — o gargalo real era P0.1.
- `compute_layout_signature` O(n) por ciclo: é o que GARANTE ciclos baratos; manter.
- Forks frios (OSD `notify-send`, game-mode `makoctl`/`paplay`, spawn de apps): por
  transição, não por frame — ok.

## Ordem de execução sugerida

1. **Agora (sem colisão):** P0.1 → P1.1 → P0.2 → P3.3a/b → P3.4 (todos bar/menu/icons).
2. **Após o diff em fluxo estabilizar:** P0.4 (verificar→fix) → P1.2 → P1.3 (via review
   da outra sessão) → P2 (absorvendo `window_is_ignored`).
3. **Depois:** P3.1, P3.2, P3.3d (decisão cache vs daemon), colaterais C1-C6.

Build/validação por item: `ninja -C build` + `tools/build-deploy.sh` + teste manual do
comportamento descrito em cada "Verificação". Itens P0.1/P0.2 têm verificação objetiva
por strace/SNI-fake; P0.4 exige medição em log ANTES do fix (não instalar às cegas).

---

## 📋 Checklist de Controle de Implementação

Use esta lista para marcar os itens à medida que forem implementados e testados.

### P0 — Latência Interativa
- [x] **1.** **P0.1.1** — Implementar cache dinâmico de ícones (sem limite duro) com cacheamento de misses (NULL) no resolvedor em [bar-icons.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-icons.c).
- [x] **2.** **P0.1.2** — Memoizar a `cairo_surface_t*` no próprio `struct App` (campo `icon_surface`) no menu ([maindeck-menu.c](file:///home/tcfialho/Documents/poc/maindeck-wm/maindeck-menu.c)) resolvida de forma lazy.
- [x] **3.** **P0.2** — Migrar chamadas bloqueantes de D-Bus (`GetAll`/`GetLayout`) para chamadas assíncronas no tray ([bar-tray.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-tray.c)).
- [x] **4.** **P0.3** — Substituir fork+exec do `pkill` ao clicar em janelas por envio de datagrama Unix-socket/UDP ou kill direto ao PID no [wm-input.c](file:///home/tcfialho/Documents/poc/maindeck-wm/wm-input.c).
- [ ] **5.** **P0.4** — Adicionar chamada de `river_window_manager_v1_manage_dirty()` no timeout do timer de hold no [wm-input.c](file:///home/tcfialho/Documents/poc/maindeck-wm/wm-input.c) (após validação de log).

### P1 — Corretude Estrutural
- [ ] **6.** **P1.1** — Refatorar o array de toplevels da taskbar ([bar-taskbar.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-taskbar.c)) para slots estáveis (ponteiros alocados via calloc) para evitar ponteiros corrompidos após `memmove`.
- [x] **7.** **P1.2** — Limpar `seat->interacted` e `seat->hovered` em `window_destroy_closed` no [wm-handlers.c](file:///home/tcfialho/Documents/poc/maindeck-wm/wm-handlers.c) para evitar use-after-free (UAF).
- [ ] **8.** **P1.3** — Incluir o bit de `window_is_ignored` na assinatura de dedup do layout (`compute_layout_signature`) no [wm-handlers.c](file:///home/tcfialho/Documents/poc/maindeck-wm/wm-handlers.c).

### P2 — Otimização do Layout do WM
- [x] **9.** **P2** — Substituir chamadas O(n²) no layout materializando uma `struct LayoutView` em passada única nos ciclos de layout no [wm-layout.c](file:///home/tcfialho/Documents/poc/maindeck-wm/wm-layout.c).

### P3 — Render e Startup
- [ ] **10.** **P3.1** — Implementar damage parcial por seção alterada na barra ([bar-surface.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-surface.c)).
- [ ] **11.** **P3.2** — Cachear `PangoLayout` por toplevel (taskbar) e para o relógio na barra ([bar-render.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-render.c)).
- [ ] **12.** **P3.3a** — Alterar o redimensionamento da lista de apps no menu para duplicar a capacidade (capacidade ×2) em vez de `napps+1`.
- [ ] **13.** **P3.3b** — Otimizar parse dos arquivos `.desktop` no menu para parsear in-place no buffer.
- [ ] **14.** **P3.3d** — Implementar cache serializado em disco (`~/.cache/maindeck/apps.cache`) para o menu.
- [ ] **15.** **P3.4** — Substituir comparações de string (`strcmp`) por enums carregados no startup para a renderização de status.

### Achados Colaterais
- [ ] **16.** **C1** — Fazer o ícone de volume ler o volume/mute real do sistema em vez de valores fixados no código ([bar-render.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-render.c)).
- [ ] **17.** **C2** — Usar o tema de ícones especificado no arquivo de configuração em vez de fallback estático ([bar-config.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-config.c) / [bar-icons.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-icons.c)).
- [ ] **18.** **C3** — Adicionar suporte a eventos de `NewIcon` e `NewTitle` no tray ([bar-tray.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-tray.c)).
- [ ] **19.** **C4** — Remover a função morta `make_layout` em [bar-render.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-render.c).
- [ ] **20.** **C5** — Remover o campo inutilizado `seat->hovered` (ou implementar seu uso efetivo).
- [ ] **21.** **C6** — Evitar chamada síncrona de `wl_display_roundtrip` dentro do fluxo de render do menu.

