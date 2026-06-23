# Animações do Deck: Deck Switch e Deck Close — fluxo maindeck-wm → river

Documento de análise do caminho completo de execução, **desde a tecla na
maindeck-wm até o tween no compositor river**, para os comandos:

- **Deck Switch** — `Win+Right`, `Win+Left` (e o caso especial `Win+Tab+Tab`,
  que **na prática NÃO é deck switch** — ver seção abaixo)
- **Deck Close** — `Win+Delete` / `Alt+F4` / `Win+F4`

Repositórios:
- WM: `maindeck-wm` (C)
- Compositor: `references/river/river` (Zig) — doc espelho em `river/river/ANIM-DECK-SWITCH-E-CLOSE.md`

---

## ⚠️ REFATORAÇÃO DECLARATIVA — 2026-06-22 (em validação)

> O corpo abaixo descreve o sistema **ANTIGO** (animação inferida por diff de
> geometria no relayout + direção de close inferida por geometria no river). Esse
> design foi **substituído** por um modelo declarativo. Plano:
> `~/.claude-msn/plans/declarative-watching-pretzel.md`.

**Novo princípio: a AÇÃO declara o enum; o river só executa. Sem inferência.**

1. **A ação decide** (`wm-input.c` `seat_action`): cada `ACTION_*` grava
   `wm.pending_anim` (ex.: `DECK_NEXT/PREV → SLIDE_DECK_OUT`, `SWAP → SPRING`,
   `MAXIMIZE/RESTORE → REFLOW_EASE`, `MINIMIZE/UNMINIMIZE → MINIMIZE/UNMINIMIZE`).
2. **O relayout transmite, não infere** (`wm-layout.c` `window_render_layout`):
   o bloco antigo `size_changed→swap / pos_changed<50→nudge` **foi removido**;
   agora emite `wm.pending_anim` para a janela que mudou de geometria. `pending_anim`
   é one-shot (zerado no fim do render cycle, `wm-handlers.c`).
3. **Close pré-registrado** (novo request de protocolo `set_close_intent`, since=7):
   a cada relayout o WM grava em cada janela seu close por papel
   (`md_send_close_intent`: deck→SLIDE_DECK_OUT, main-com-deck→SLIDE_CLOSE,
   solo→FADE_CLOSE). O river **lê** esse `close_intent` no `unmap` —
   `minOtherVisibleX`/comparação de `box.x` **removidos**. Resolve o close
   iniciado pelo cliente (que o river processa antes do WM reagir) sem geometria.
4. **River arma o tween por enum** (`Window.zig`): a entrada no slot do deck
   (`SLIDE_IN`) agora arma `armSlide` mesmo quando a janela já era posicionada
   (`first_show=false`) — **conserta o deck-switch que só mostrava o nudge de foco**.
5. **Win+Tab+Tab corrigido** (`wm-input.c`): double-tap de `TOGGLE_TARGET` agora é
   resolvido também no **release** (via `resolve_toggle_tap`), então funciona em
   bindings com `hold_action` (Win+Tab tem hold=SWAP). Antes o early-return do
   `pressed` tornava o double-tap inalcançável.

**Protocolo:** `river_window_v1`/`river_window_manager_v1` v6→**v7**;
`scanner.generate("river_window_manager_v1", 7)` em `build.zig` (ver memória
`project_zig_wayland_scanner_version_gotcha`).

Evidência do bug original: `docs/evidencia-wm-2026-06-22.log` (WM emitia os intents
certos; a falha estava no river não armar o slide).

---

## Princípio central

**Nenhum atalho dispara uma animação diretamente.** O WM:

1. apenas **reordena a lista encadeada** de janelas e/ou muda `target_index`, e
   marca o estado como sujo (`focus_dirty` / `manage_dirty`);
2. depois, num **render cycle**, recalcula o layout e **detecta a animação
   comparando a geometria nova de cada janela com a anterior** (`last_render_x/w`),
   emitindo um único request — `set_animation_intent` — por janela que mudou.

O **river** apenas **executa** o tween: interpola localmente dentro do frame loop
do output, sem round-trip Wayland por frame.

### Modelo de layout (2 slots fixos)

`layout_view_init()` (`wm-layout.c:152`; geometria dos slots em `:174-197`):

- **MAIN** = 2/3 da largura, à esquerda — lista `index 0`
- **DECK** = 1/3 da largura, à direita — lista `index 1`
- `index >= 2` = **hidden** (pilha fora da tela, à direita)

> Largura **efetiva** não é exatamente 2/3 e 1/3: há um `GAP/2` entre os slots
> (`wm-layout.c:184-196`) e `window_render_layout` ainda subtrai `BORDER_WIDTH*2`
> da largura / `BORDER_WIDTH` da altura (`:924-927`). O `x` aplicado vira
> `box.x + BORDER_WIDTH` (≈3px) — relevante para a inferência de close do
> compositor (o main não fica em `x=0`, e a direção é decidida contra o **menor
> x** das outras janelas, não contra `x>0`).

Navegar no deck = girar quem ocupa o slot 1.

---

## Pipeline comum (lado WM)

```
tecla
  → river_xkb_binding_v1.pressed            (wm-input.c:69)
  → seat->pending_action = ACTION_…
  → manage cycle (wm-handlers.c:758):
      seat_manage() → seat_action()          (wm-input.c:586 / :434)
        → md_deck_next/prev / swap / close    (muda a LISTA + focus_dirty=true)  ← NÃO anima aqui
      → compute_layout_signature() sig mudou? (wm-handlers.c:796-800)
      → window_manage_layout() por janela     (propõe dimensões via propose_dimensions)
  → render cycle (wm_handle_render_start, wm-handlers.c:828):
      window_render_layout() por janela       (wm-layout.c:779)
        → compara new_x/new_w vs last_render_x/last_render_w
        → md_send_animation_intent(window, INTENT)   ← AQUI a animação é disparada
        → river_node_v1_set_position(...)
```

`md_send_animation_intent()` (`wm-animation-intents.c:128`) é o **único** request
que carrega a animação. Manda `(intent, duração, easing)` via
`river_window_v1_set_animation_intent` (request `since=6`); o compositor aplica no
**próximo `render_finish`** daquela janela. Por isso precisa sair no **mesmo
render cycle** da mudança de geometria — exatamente onde `window_render_layout` o
chama.

> Compositor mais antigo (proxy `< 6`): o request é pulado silenciosamente
> (`wm-animation-intents.c:137`) — as animações simplesmente não disparam.

---

## Como o intent é escolhido — por GEOMETRIA, não pela tecla

`window_render_layout()` (definida em `wm-layout.c:779`; bloco das janelas
tiladas em `:904-948`), para cada janela tilada:

- **`was_visible != visible`** (mudou de visibilidade):
  - virou visível → `md_intent_for_open(...)`  (`wm-layout.c:909`)
  - sumiu → `md_intent_for_close(index, ...)`  (`wm-layout.c:911`)
- **`was_visible && visible`** (continua visível, geometria mudou):
  - `size_changed` → `REFLOW_EASE`; **mas** se `index < 2 && visible_count >= 2`
    → **`SWAP`/SPRING** (`wm-layout.c:935-936`). A condição `index < 2` importa:
    um slot hidden (`index ≥ 2`) com mudança de tamanho pegaria `REFLOW_EASE`, não
    `SWAP` (na prática hidden não chega aqui por estar invisível).
  - senão `pos_changed` → `deck_move` (`REFLOW_EASE`); se o deslocamento for
    `< 50px` → `NUDGE`  (`wm-layout.c:938-943`)
- **nada mudou** → **nenhum intent, nenhuma animação** (`:945`)

Helpers (`wm-animation-intents.c`):

- `md_intent_for_close(index, count_before)` (`:184`):
  `count_before==1` → `FADE_CLOSE`; `index==1` (deck) → **`SLIDE_DECK_OUT`**;
  senão → `SLIDE_CLOSE`.
- `md_intent_for_open(count_before)` (`:173`):
  `count_before==0` → `FADE_OPEN`; senão → **`SLIDE_IN`**.

Tabela de duração/easing por intent: `wm-animation-intents.c:73-102`
(`SLIDE_DECK_OUT` = 200ms, `EASE_IN`; `SLIDE_IN` = 200ms, `EASE_OUT`;
`SPRING` = 280ms, `CUBIC_SPRING`).

---

## DECK SWITCH

### Win+Right = `ACTION_DECK_NEXT` → `md_deck_next()`

Binding: `wm-input.c:549`. Implementação: `wm-layout.c:490`.

O que o WM faz na lista: remove a janela do slot 1 (deck) e a **reinsere no fim
da região visível** (`wl_list_remove` + `wl_list_insert` na cauda);
`target_index = 1`. A janela do deck vira *hidden* e a próxima oculta sobe para o
slot do deck.

O que o relayout detecta e dispara:
- janela que **sai** do slot deck: `was_visible && !visible` →
  `md_intent_for_close(index=1)` → **`SLIDE_DECK_OUT`** (`wm-layout.c:911`)
- janela que **entra** no slot deck: `!was_visible && visible` →
  `md_intent_for_open(count>1)` → **`SLIDE_IN`** (`wm-layout.c:909`)
- **main**: geometria idêntica → **nenhum intent**

Requer ≥3 janelas. Com ≤2, `md_deck_next` aborta com OSD
*"sem janela invisível à direita"* (`wm-layout.c:491`).

### Win+Left = `ACTION_DECK_PREV` → `md_deck_prev()`

Binding: `wm-input.c:550`. Implementação: `wm-layout.c:503`.

Espelho do anterior: traz a última janela visível para logo após o main
(`move_after`), virando o novo deck; `target_index = 1`. Mesmos intents
(`SLIDE_IN` / `SLIDE_DECK_OUT`), direção oposta.

### Win+Tab+Tab = `ACTION_DECK_NEXT` (NÃO é swap!)

> **⚠️ CORREÇÃO (auditoria 2026-06-22).** A versão anterior deste doc afirmava
> que Win+Tab+Tab dispara `ACTION_DECK_NEXT` (igual a Win+Right). **Isso está
> errado.** O binding Win+Tab é criado com `hold_action = ACTION_SWAP_MAIN_DECK`
> (`wm-input.c:548`, 2º argumento ≠ `ACTION_NONE`). Em
> `xkb_binding_handle_pressed` (`wm-input.c:72-76`), quando `hold_action !=
> ACTION_NONE` a função **arma o timer de hold e retorna na linha 76**, ANTES de
> chegar ao bloco de double-tap (`wm-input.c:85-104`). O `tap_action`
> (`ACTION_TOGGLE_TARGET`) é entregue no **release** (`xkb_binding_handle_released`,
> `wm-input.c:117`). Resultado: **Win+Tab+Tab = `ACTION_TOGGLE_TARGET` duas
> vezes** — alterna o foco main↔deck e volta; **não** roda `md_deck_next` e
> **não** produz o carrossel do deck.

O bloco de double-tap que produz `ACTION_DECK_NEXT` (`wm-input.c:85-104`,
`DOUBLE_TAP_MS = 280` em `wm-input.c:25`) só é alcançável por bindings **sem**
`hold_action` cujo `tap_action == ACTION_TOGGLE_TARGET`. No keymap atual isso é
**`Alt+F23`** (`wm-input.c:541`) e o **pointer Super+clique-esquerdo**
(`wm-input.c:567`) — **não** o Win+Tab. O comentário no código
(`wm-input.c:79` "Win+Tab double-tap → cycle the DECK") é **enganoso/obsoleto**:
descreve uma intenção que o binding atual, por ter `hold_action`, não cumpre por
esse caminho.

> **Logo, apenas Win+Right e Win+Left** disparam o carrossel do deck
> (`DECK_NEXT`/`DECK_PREV`). Win+Tab é alternância de foco (1 tap) ou swap
> main↔deck (hold).

**Resultado visual de Win+Right / Win+Left:** carrossel lateral dentro do slot do
deck — a janela atual escorrega para fora à direita (`SLIDE_DECK_OUT`) enquanto a
próxima entra deslizando pela esquerda (`SLIDE_IN`), **main imóvel**.

---

## DECK CLOSE = `ACTION_CLOSE_TARGET` (Win+Delete / Alt+F4 / Win+F4)

Bindings: `wm-input.c:558` (Win+Delete), `:559` (Alt+F4) e `:560` (**Win+F4** —
terceiro binding de close). Despacho: `seat_action`, `wm-input.c:446-458`.

Aqui o WM faz algo **diferente** dos switches acima. Ele **não** reordena a
lista nem envia intent. O handler (`wm-input.c:446-456`) primeiro trata um caso à
parte: se há uma **janela flutuante focada** (`seat->focused->floating &&
!closed`), fecha ela e sai; **só** se não houver flutuante focada é que fecha o
target tilado, chamando direto:

```c
river_window_v1_close(target->obj);   // wm-input.c:454
wm.maximized = false;                  // wm-input.c:455
```

Isso pede ao cliente que feche. Quando o cliente fecha, o compositor faz
`unmap()` **sincronamente, ANTES** de o evento `closed` chegar ao WM. O handler do
WM `window_handle_closed` (`wm-handlers.c:179`) só marca `window->closed = true`
— **não envia `SLIDE_DECK_OUT`** para a janela que sai (não há tempo).

Então **quem decide a animação de close é o COMPOSITOR, por inferência
geométrica** (`river/river/Window.zig:1462-1486`):

- nenhuma outra janela visível → **fade + shrink** no lugar (close solo)
- `window.box.x > other_x` (está à direita) → **slide_right** (= é o deck)
- senão → **slide_left** (= é o main)

Um intent do WM, quando presente, **sobrepõe** a inferência — mas no close
iniciado pelo cliente ele nunca chega a tempo. No relayout seguinte, o WM só
emite intents para as **sobreviventes** (ex.: o main crescendo para preencher o
espaço liberado, via clip-reveal).

---

## Lado river (execução) — resumo

Detalhes completos no doc espelho do repo river. Pontos de ligação:

- Vocabulário compartilhado de intents: `river/AnimationIntent.zig` ↔
  `wm-animation-intents.h` (mesmos valores `0x0`..`0xb`).
- `Window.zig:829` recebe `set_animation_intent` → guarda em
  `rendering_requested`. Janela viva: `Window.zig:1145-1214` só arma o tween se
  `moved or resized`.
- `SWAP`/reflow → `armMove` (interpola **só posição**, easing `.spring`; NÃO
  escala o buffer do cliente — `setDestSize` na superfície viva corria com os
  commits do cliente e deformava janelas no swap; `Window.zig:1156-1160`).
- `SLIDE_IN` / `SLIDE_DECK_OUT` → `armSlide` (painel sólido, opacidade e escala
  fixas em 1.0).
- Close que sobrevive à destruição da janela: `OrphanClose`
  (`Animation.zig:575-716`) — fotografa os buffers numa árvore standalone na
  layer `close_overlay`, anima e auto-destrói. `CloseStyle`: `fade` (solo,
  shrink p/ `close_scale=0.65`) / `slide_right` (deck) / `slide_left` (main).
- Defaults river: close fade `close_anim_ms=180`, slide `slide_close_ms=200`,
  `close_scale=0.65` (`Window.zig:37-49`); spring = `cubic-bezier(0.22,1,0.36,1)`.
  O easing do slide/fade de close **não é fixado** nessas constantes — vem do WM
  via protocolo, com fallback `.ease_in` (`Window.zig:1495`).

---

## Tabela-resumo

| Atalho        | Ação WM        | O que o WM faz na lista              | Quem anima                | Intent                          |
|---------------|----------------|-------------------------------------|---------------------------|---------------------------------|
| Win+Right     | `DECK_NEXT`    | deck → cauda oculta                  | WM (relayout)             | `SLIDE_DECK_OUT` + `SLIDE_IN`   |
| Win+Left      | `DECK_PREV`    | última → após main                   | WM (relayout)             | `SLIDE_DECK_OUT` + `SLIDE_IN`   |
| Win+Tab (tap) | `TOGGLE_TARGET`| nada (só muda foco main↔deck)        | —                         | nenhum (ou `NUDGE` se reposicionar) |
| Win+Tab+Tab   | `TOGGLE_TARGET`×2 | nada (alterna foco e volta)       | —                         | **NÃO é deck switch** (ver seção) |
| Win+Tab (hold)| `SWAP_MAIN_DECK` | troca main↔deck (`move_first`)    | WM (relayout)             | `SPRING` (2 janelas trocam de lugar/tamanho) |
| Deck Close    | `CLOSE_TARGET` | nada (só `river_window_v1_close`)    | **compositor (inferência)** | `slide_right` inferido        |

**Diferença essencial:** no *deck switch* o WM controla por intent explícito
(diff de geometria no render cycle); no *deck close* o WM só pede o close e o
**compositor infere** a direção, porque a janela já foi destruída antes de o WM
poder falar.

---

## Verificação ao vivo

`md_verbose` emite no log:
`anim intent: window="…" intent=… duration=… ms easing=…`
(`wm-animation-intents.c:143-148`).

Scripts de teste no repo: `anim-test-swap.sh`, `close-deck-test.sh`,
`close-pixel-test.sh`.
