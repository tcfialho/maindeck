# Auditoria de Animações: MainDeck / River vs. Protótipo P17

Este documento apresenta a análise comparativa entre a especificação declarativa do Window Manager e a implementação prática das animações, confrontando o protótipo com os códigos reais do compositor e do gerenciador de janelas.

---

## 📂 Arquivos Envolvidos na Auditoria

1. **Especificação de Animações (WM):**
   * [anim-deck-switch-e-close.md](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/anim-deck-switch-e-close.md)
2. **Protótipo de Referência (UX/UI):**
   * [prototype-anim-p17.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p17.html)
3. **Código-Fonte do Compositor (River):**
   * [Window.zig](file:///home/tcfialho/Documents/poc/references/river/river/Window.zig)
   * [AnimationIntent.zig](file:///home/tcfialho/Documents/poc/references/river/river/AnimationIntent.zig)
4. **Código-Fonte do Window Manager (WM):**
   * [wm-animation-intents.c](file:///home/tcfialho/Documents/poc/maindeck-wm/wm-animation-intents.c)

---

## 1. Comparativo da Arquitetura de Execução

| Característica | Implementação no Compositor (River / WM) | Implementação no Protótipo (P17 HTML) |
| :--- | :--- | :--- |
| **Mecanismo de Trigger** | Declarativo via `window->pending_anim` per-window. O relayout Wayland envia o intent pela wire (`river_window_v1`/`river_window_manager_v1` v7). | Mutações de estado JS (`wm.windows`) disparando redefinições de classes CSS e recalculo de estilos via `applyLayout()`. |
| **Controle de Timing e Easing** | O River **lê e respeita** a duração e o easing enviados pela wire. No entanto, mapeia `EASING_EASE_IN_OUT` (0x3) para `.ease_out` (via `animationEasingFromProtocol`), e a duração de `280ms` (do `REFLOW_EASE`) vem declarada pelo WM na wire. Apenas swaps (`SPRING`) usam `.spring` (0x4). | Uso extensivo de transições CSS gerais (`ease-in-out 0.20s`) sobrepostas por keyframes dedicados com durações variadas (`0.13s` a `0.24s`). |
| **Gerenciamento de Overlap (Clipping)** | Renderizador do compositor deve aplicar clipping geométrico na divisa do deck. | Uso de `clip-path: inset(...)` dinâmico nos keyframes de slide para impedir que animações do deck invadam o slot MAIN. |

---

## 2. Auditoria Detalhada por Ação e Animação

### A. Troca de Deck (Deck Switch)
*   **Comandos:** `Win+→` (`DECK_NEXT`), `Win+←` (`DECK_PREV`), double-tap `Win+Tab+Tab`.
*   **Comportamento Real (WM / River):**
    *   `DECK_NEXT` declara `SLIDE_DECK_OUT` (sai) e `DECK_IN_LEFT` (entra pela esquerda com clipping).
    *   `DECK_PREV` declara `SLIDE_DECK_OUT_LEFT` (sai pela esquerda) e `DECK_IN_RIGHT` (entra pela direita com clipping).
    *   **Saída Orphan:** O `SLIDE_DECK_OUT`/`_LEFT` é interceptado pelo River e roda como um orphan a **`130ms` `.ease_in`** hardcoded no compositor (`Window.zig:1077`).
    *   **Entrada e Reflow:** As janelas que entram e refluem usam `REFLOW_EASE` que envia `EASING_EASE_IN_OUT` (0x3), mapeado no compositor para **`.ease_out` amortecido de `280ms`**.
*   **Comportamento no Protótipo (P17):**
    *   As janelas de saída usam keyframe rápido de `130ms` (`ease-in`).
    *   As janelas de entrada usam keyframe de `200ms` (`ease-out`).
*   > [!NOTE]
    *   **Alineamento de Easing & Timing**: Graças ao hardcode de `130ms` `.ease_in` na saída orphan do River e ao mapeamento de `REFLOW_EASE` para `.ease_out` amortecido de `280ms` (em vez de elástico), o comportamento de transição do River é extremamente similar e alinhado ao protótipo (com saídas rápidas a 130ms e entradas amortecidas próximas a 200ms).

### B. Fechamento de Janelas (Close Target)
*   **Comandos:** `Win+Delete` / `Alt+F4` / `Win+F4`.
*   **Comportamento Real (WM / River):**
    *   Baseia-se em pre-registro (`set_close_intent`).
    *   Deck close → `SLIDE_DECK_OUT`.
    *   Main com Deck close → `SLIDE_CLOSE`.
    *   Solo close → `FADE_CLOSE`.
    *   Sobrevivente solo sofre `GROW_REVEAL` declarativo.
*   **Comportamento no Protótipo (P17):**
    *   **Solo Close:** Encolhe no centro (`scale(1)` → `scale(0.65)`) com fade out em `200ms` (`anim-close-scale`).
    *   **Deck Close (com MAIN presente):** Translada `100%` sólido para a direita, sem fade, em `200ms` (`anim-deck-close-right`).
    *   **MAIN Close (com DECK presente):** Translada `100%` sólido para a esquerda, sem fade, em `200ms` (`anim-close-solo`).

### C. Troca de Slots (Swap Main ↔ Deck)
*   **Comandos:** `Win+Tab` (hold/hold-release).
*   **Comportamento Real (WM / River):**
    *   Dispara o intent `SPRING` para ambas as janelas no relayout, que envia `EASING_CUBIC_SPRING` (0x4).
    *   O River interpreta como `.spring` genuíno (280ms).
*   **Comportamento no Protótipo (P17):**
    *   Ação `swapGeom()` congela transições, troca posições nos slots físicos e reabilita a transição no rAF seguinte.
    *   CSS resolve a animação de tamanho e posição usando `ease-in-out` de `200ms`.
*   > [!IMPORTANT]
    *   **Diferença Elástica Genuína**: Apenas a troca (`SWAP_MAIN_DECK`) usa `.spring` real (overshoot elástico). O protótipo usa `ease-in-out` suave.

### D. Minimizar e Des-minimizar (Minimize / Unminimize)
*   **Comandos:** `Win+↓` (hold) / `Win+↑` (hold).
*   **Comportamento Real (WM / River):**
    *   `MINIMIZE` (desce + encolhe + fade) e `UNMINIMIZE` (sobe + cresce + fade a partir da barra).
*   **Comportamento no Protótipo (P17):**
    *   **Minimizar:** `anim-minimize` em `200ms ease-in` translada `Y: 60px` para baixo e escala para `0.55` com fade out (origem: `bottom center`).
    *   **Des-minimizar:** `anim-unminimize` em `220ms ease-out` translada de `Y: 60px` para `0` e escala de `0.55` para `1` com fade in (origem: `bottom center`).

### E. Abertura de Novo Aplicativo (Open Application)
*   **Comportamento Real (WM / River):**
    *   Se solo → `FADE_OPEN`.
    *   Se em grupo → `SLIDE_IN` para a nova janela; as janelas empurradas para hidden somem instantaneamente.

---

## 3. Conclusões e Recomendações para a Implementação

1.  **Alinhamento de Easing Real (Mais Próximo do Protótipo)**:
    *   Diferente do inicialmente estimado, o River **não** aplica `.spring` elástico nas ações de maximizar, restaurar ou no reflow do deck switch. Como elas usam `REFLOW_EASE` que envia `EASING_EASE_IN_OUT` (0x3), elas são mapeadas para `.ease_out` amortecido de `280ms`. Portanto, o comportamento real é muito mais similar ao `ease-in-out` do protótipo (amortecido) do que a um spring vibracional. O único spring real é o `SWAP_MAIN_DECK` (`0x4`).
2.  **Mecanismo de Duração / Easing**:
    *   O River respeita e processa a wire e as durações do protocolo Wayland (`rendering_requested.animation_duration_ms`). A duração de `280ms` ou o fallback de easing `.spring` só ocorrem quando a wire não especifica uma configuração válida.
3.  **Replicação do Clipping no Deck**:
    *   É essencial que o River replique o comportamento de recorte (`clip-path` do protótipo) ao animar transições direcionais no deck, mantendo a estética confinada ao deck.

---

## 4. Tabela Comparativa Detalhada por Item / Atalho

| Ação (Atalho) | Intent Declarado (WM/River) | Animação no Protótipo P17 | Diferenças Encontradas (Auditoria Real) |
| :--- | :--- | :--- | :--- |
| **DECK_NEXT** (`Win+→` / `Win+Tab+Tab`) | sai→`SLIDE_DECK_OUT` (dir)<br>entra→`DECK_IN_LEFT` (esq + clip) | Out: `130ms` dir (`ease-in`) <br>In: `200ms` esq (`ease-out` + `clip-path`) | **IGUAL** na saída (River usa `130ms` `.ease_in` hardcoded no orphan). A entrada no River roda a `280ms` `.ease_out` (REFLOW_EASE), que é amortecido. |
| **DECK_PREV** (`Win+←`) | sai→`SLIDE_DECK_OUT_LEFT` (esq)<br>entra→`DECK_IN_RIGHT` (dir + clip) | Out: `130ms` esq (`ease-in` + `clip-path`) <br>In: `200ms` dir (`ease-out`) | Mesma dinâmica do `DECK_NEXT` (saída orphan 130ms ease-in, entrada 280ms ease-out). |
| **TOGGLE_TARGET** (`Win+Tab` tap) | Nenhum (River faz `NUDGE` no focus-gained) | `anim-nudge-right` / `anim-nudge-left` (`160ms` `translateX(8px)`) | **IGUAL** |
| **SWAP_MAIN_DECK** (`Win+Tab` hold) | Ambas→`SPRING` | Troca física via CSS Transition `ease-in-out` de `200ms` | **Genuinamente elástica**. O River usa `.spring` real (0x4, 280ms) gerando overshoot elástico, enquanto o protótipo usa transição suave. |
| **MAXIMIZE_TARGET** (`Win+↑` tap) | `REFLOW_EASE` | Cresce p/ tela cheia instantaneamente; outra encolhe e faz fade out (`240ms` hidden) | O River utiliza easing `.ease_out` amortecido (`280ms`), muito próximo do `ease-in-out` de `200ms` do protótipo (sem spring). |
| **RESTORE** (`Win+↓` tap) | `REFLOW_EASE` | Encolhe p/ slot original; a oculta reaparece com fade in / cresce | O River utiliza easing `.ease_out` amortecido (`280ms`), similar ao do protótipo. |
| **MINIMIZE_TARGET** (`Win+↓` hold) | `MINIMIZE` | `anim-minimize` (`200ms` `translateY(60px)` + `scale(0.55)` + fade) | **IGUAL** |
| **UNMINIMIZE** (`Win+↑` hold) | `UNMINIMIZE` | `anim-unminimize` (`220ms` `translateY(0)` + `scale(1.0)` + fade in) | **IGUAL** |
| **CLOSE_TARGET** (`Win+Del` / `Alt+F4`) | unmap com close intents registrados | Solo: fade + encolhe (`scale(0.65)`) <br>Main/Deck com par: slides sólidos direcionais (`100%`) | O protótipo simplificou eliminando fade nas transições com múltiplos slots, usando apenas slide sólido lateral de 100%. |
| **Abrir Aplicativo** (Novo Window) | solo→`FADE_OPEN`<br>grupo→`SLIDE_IN` | Solo: cresce no centro (`0.65 → 1` em `220ms`) <br>Grupo: slide sólido de `-45%` para `0%` em `200ms` | **IGUAL** |
| **PROMOTE_TO_MAIN** (`Win+←` hold) | `REFLOW_EASE` | Inverte slots e roda transição CSS de geometria `ease-in-out 200ms` | O River utiliza `.ease_out` amortecido (`280ms`), similar ao `ease-in-out` do protótipo. |
