# Animações do Deck: Deck Switch e Deck Close — fluxo maindeck-wm → river

Documento de análise do caminho completo de execução, **desde a tecla na
maindeck-wm até o tween no compositor river**, para os comandos:

- **Deck Switch** — `Win+Right`, `Win+Left`, `Win+Tab+Tab` (double-tap)
- **Deck Close** — `Win+Delete` / `Alt+F4` / `Win+F4`

Repositórios:
- WM: `maindeck-wm` (C)
- Compositor: `references/river/river` (Zig) — doc espelho em `river/river/ANIM-DECK-SWITCH-E-CLOSE.md`

---

## Princípio de Funcionamento (Declarativo)

**A AÇÃO declara o enum PER-WINDOW; o river arma o tween pelo enum. ZERO inferência geométrica no caminho tiled. ZERO fallback.**

1. **A ação marca PER-WINDOW** (`wm-layout.c`/`wm-input.c`): cada ação grava o
   intent na(s) janela(s) que ela afeta, em `window->pending_anim` (campo na struct
   `Window`), NÃO num global ambíguo:
   - `md_deck_next`: sai→`SLIDE_DECK_OUT`, entra→`DECK_IN_LEFT`
   - `md_deck_prev`: sai→`SLIDE_DECK_OUT_LEFT`, entra→`DECK_IN_RIGHT`
   - `md_swap_main_deck`: as DUAS janelas→`SPRING`
   - `md_insert_new_window`: só a nova→`FADE_OPEN` (solo) / `SLIDE_IN` (grupo)
   - `md_minimize_window`→`MINIMIZE`; `md_unminimize` (a que volta)→`UNMINIMIZE`
   - `mark_visible_tiled_anim(REFLOW_EASE)` p/ maximize/restore/toggle/promote/send
   - `md_mark_grow_survivor_if_lone`→`GROW_REVEAL` no sobrevivente solo do close
2. **O relayout SÓ transmite** (`window_render_layout`): emite `window->pending_anim`
   (one-shot, consumido) para a janela que cruza visibilidade OU muda de geometria.
   Os helpers `md_intent_for_open/close` e o `wm.pending_anim` global foram
   removidos do caminho tiled (restam só no bloco transient/floating de diálogos).
   Sem intent declarado → não anima (ex.: a janela empurrada p/ hidden ao abrir a
   3ª janela some sem animação, igual ao protótipo).
3. **Close pré-registrado** (`set_close_intent`, since=7): a cada relayout o WM grava
   o close por papel em cada janela (`md_send_close_intent`: deck→SLIDE_DECK_OUT,
   main-com-deck→SLIDE_CLOSE, solo→FADE_CLOSE). O river lê no `unmap` — sem
   inferência de `box.x`. O grow-reveal do sobrevivente é DECLARADO (`GROW_REVEAL`),
   não mais inferido por `grew && visibleManagedCount()==1` (função removida).
4. **River arma o tween PELO ENUM** (`Window.zig` `renderFinish`): `switch(intent)`
   direto — deck-in/out por intent (sem `box.x`), minimize/deck-out via orphan tree,
   grow-reveal por `intent==.grow_reveal`. O `moved or resized` permanece SÓ como
   gate de entrada ("há delta a animar?").
   > **Nota de Implementação (Limitação do Compositor):** O easing e a duração
   > enviados pelo WM na wire só são aplicados em transições de **abertura (open)** e
   > **fechamento (close)**. Para as animações de layout como troca e reflow (`SPRING`,
   > `REFLOW_EASE`), o river ignora os valores da wire e usa constantes hardcoded
   > (`move_anim_ms = 280` com easing `.spring`).
   Geometria que permanece (legítima, não é escolha): `preserve`
   (cliente commita buffer real no meio de fade_open/unminimize), entrance-slide
   guard, e a aritmética `old_box→new_box` (endpoints do tween).

**Protocolo:** `animation_intent` enum vai até `0xf` (`grow_reveal`); novos: `slide_deck_out_left 0xc`, `deck_in_right 0xd`, `deck_in_left 0xe`, `grow_reveal 0xf`. `river_window_v1`/`river_window_manager_v1` v7; WM binda em v7 (`set_close_intent`). Os 2 XMLs (river + maindeck-wm) são cópias idênticas.

**Commits de referência:** maindeck-wm `892005b`/`a51fd77`/`e855bbc`/`dce16d7`; river `5b37d0a`/`6f98dc7`/`860486e`.

---

## Tabela-resumo (implementação declarativa)

Todos os intents são **declarados pela ação per-window** (`window->pending_anim`); o river arma o tween pelo enum, sem inferência geométrica. "Quem marca" = a função que grava o intent.

| Atalho | Ação WM | O que o WM faz na lista | Quem marca o intent | Intent(s) declarado(s) |
|---|---|---|---|---|
| **Win+→** | `DECK_NEXT` | deck → cauda oculta; próxima oculta sobe | `md_deck_next` (per-window) | sai `SLIDE_DECK_OUT` (dir) · entra `DECK_IN_LEFT` (esq+clip) |
| **Win+←** | `DECK_PREV` | última visível → após main | `md_deck_prev` (per-window) | sai `SLIDE_DECK_OUT_LEFT` (esq) · entra `DECK_IN_RIGHT` (dir+clip) |
| **Win+Tab** (tap) | `TOGGLE_TARGET` | nada (só foco main↔deck) | — | nenhum (river faz `NUDGE` no focus-gained) |
| **Win+Tab** (hold) | `SWAP_MAIN_DECK` | troca main↔deck (`move_first`) | `md_swap_main_deck` (as 2) | `SPRING` em main **e** deck |
| **Win+↑** (tap) | `MAXIMIZE_TARGET` | `wm.maximized=true` | `mark_visible_tiled_anim` | `REFLOW_EASE` (target cresce, outra some) |
| **Win+↑** (hold) | `UNMINIMIZE` | desminimiza LIFO → vira MAIN | `md_unminimize` (a que volta) | `UNMINIMIZE` (sobe+escala+fade, origem base) |
| **Win+↓** (tap) | `RESTORE` | `wm.maximized=false` | `mark_visible_tiled_anim` | `REFLOW_EASE` (target encolhe, outra volta) |
| **Win+↓** (hold) | `MINIMIZE_TARGET` | janela → cauda + `hide` | `md_minimize_window` | `MINIMIZE` (orphan: desce+escala+fade, origem base) |
| **Win+Shift** (tap) | `TOGGLE_MAXIMIZE` | alterna `wm.maximized` | `mark_visible_tiled_anim` | `REFLOW_EASE` |
| **Win+Del / Alt+F4 / Win+F4** | `CLOSE_TARGET` | `river_window_v1_close` (cliente fecha) | pré-registro `set_close_intent` + sobrevivente | sai `FADE_CLOSE`/`SLIDE_DECK_OUT`/`SLIDE_CLOSE` por papel · sobrevivente solo `GROW_REVEAL` |
| **(abrir app)** | — (novo window) | nova→MAIN; antiga main→deck; antiga deck→hidden | `md_insert_new_window` (só a nova) | solo `FADE_OPEN` · grupo `SLIDE_IN`; a que vira hidden: **nenhum** (some sem animação) |
| **Win+←** (hold) / **Ctrl+F23** | `PROMOTE_TARGET_TO_MAIN` | target → MAIN (`move_first`) | `mark_visible_tiled_anim` | `REFLOW_EASE` |
| **Win+→** (hold) / **Ctrl+F24** | `SEND_TARGET_TO_DECK_BOTTOM` | target → cauda; próxima sobe | `mark_visible_tiled_anim` | `REFLOW_EASE` |

**Funcionamento Geral:** A escolha das animações é puramente declarativa: cada ação do WM grava o intent desejado diretamente na janela afetada (`window->pending_anim`) no momento em que o comando é acionado, e o compositor river simplesmente executa o tween correspondente ao enum (`switch(intent)`). No fechamento de janelas, o WM pré-registra a animação de close (`set_close_intent`, persistente por papel) que o river lê no `unmap`, e o grow-reveal da janela sobrevivente solo é explicitamente declarado (`GROW_REVEAL`), eliminando qualquer inferência geométrica.

**Win+Tab+Tab:** Funciona como deck-switch (`ACTION_DECK_NEXT`). Embora o `Win+Tab` tenha o hold mapeado para `SWAP_MAIN_DECK`, o WM implementa detecção de double-tap (`resolve_toggle_tap` em `wm-input.c`) no release da tecla. Um double-tap rápido (dentro de `DOUBLE_TAP_MS` / 280ms) intercepta a ação e dispara `ACTION_DECK_NEXT` (deck switch), gerando as animações direcionais `SLIDE_DECK_OUT` (sai para direita) e `DECK_IN_LEFT` (entra pela esquerda).

---

## Verificação ao vivo

`md_verbose` emite no log:
`anim intent: window="…" intent=… duration=… ms easing=…`
(`wm-animation-intents.c:155-161`).

Scripts de teste no repo: `anim-test-swap.sh`, `close-deck-test.sh`, `close-pixel-test.sh`.
