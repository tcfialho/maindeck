# Diagnóstico: foco não vai para a janela filha (child/dialog centralizado)

Data: 2026-06-24
Componente: `maindeck-wm` (cliente do protocolo river-window-management-v1). Lado servidor (`river`) investigado e **inocentado** — ver §5.

> **Evidência ao vivo (sessão corrente, PID maindeck-wm=5136):** o log da sessão
> (`~/.local/share/sddm/wayland-session.log`) está repleto de
> `wlr_text_input_v3.c:190: Text input commit received without focus`. Isto é a
> assinatura direta do sintoma: o campo de texto da filha (via protocolo
> `text-input-v3`) tenta commitar texto mas **não possui foco de teclado**
> (`wl_keyboard.enter` nunca foi entregue à filha). Confirma empiricamente §4a.

## 1. Sintomas relatados

1. Há uma janela filha (child/transient) centralizada sobre a pai (caso especial nosso: diálogos modais, ex.: diálogos do WPS/Steam).
2. Movo o foco para outra janela e tento voltar:
   - **Win+Tab**: o foco vai para a janela **pai** (que está atrás), não para a filha.
   - **Clique num campo de texto da filha**: o campo **não** recebe foco de teclado.
   - **Clique em botões/elementos de seleção da filha**: funcionam normalmente.
3. (Possivelmente separado) dropdowns às vezes "somem atrás" da janela — frequente na Steam. Tratado como hipótese à parte (§6), não confirmado como o mesmo bug.

## 2. Modelo de dados relevante

`struct Window` (types.h:40) tem:
- `struct Window *parent` — não-NULL ⇒ é janela filha.
- `bool floating` — caminho de foco alternativo (satty, config-list, hint-capped).
- Uma filha tem **`parent != NULL` e `floating == false`** (garantido em `window_handle_parent`, wm-handlers.c:417-420: ao virar child, `floating=false; autofloat=false`).

`struct Seat` (types.h:146): `focused` (o que dissemos ao river focar) e `interacted` (última janela que o river reportou interação via `window_interaction`).

## 3. As três funções de índice IGNORAM filhas

Todas pulam `parent != NULL || floating`:

- `visible_window_count()` (wm-layout.c:72-79): conta só `!minimized && parent==NULL && !floating`.
- `window_at(index)` (wm-layout.c:89-98): itera pulando `parent!=NULL || floating`.
- `window_index(needle)` (wm-layout.c:132-141): **retorna -1 para qualquer filha** (continua o laço em `parent!=NULL || floating`).

Consequência: **uma filha não tem "índice MainDeck". É invisível para o sistema de target** (`target_window()` = `window_at(target_index)`, wm-layout.c:143-146). O target é sempre uma janela **raiz** (MAIN ou DECK).

Isto é intencional para o *layout* (a filha é posicionada/centralizada à parte, não ocupa slot). O problema é que o **foco** herdou a mesma cegueira.

## 4. Causa raiz — dois caminhos de foco, ambos cegos para a filha

### 4a. Caminho do CLIQUE (`window_interaction` → `seat_manage`)

Quando clico na filha, o river envia `window_interaction` com a janela **da filha** (confirmado no servidor, §5). O WM grava `seat->interacted = <filha>` (wm-input.c:388-392).

Em `seat_manage` (wm-input.c:632-646):
```c
if (seat->interacted != NULL) {
    int32_t idx = window_index(seat->interacted);   // FILHA → -1
    if (idx == 0 || idx == 1) {                      // FALHA (idx == -1)
        ... target_index = idx; focus_dirty = true;
    } else if (seat->interacted->floating) {         // FALHA (filha não é floating)
        river_seat_v1_clear_focus(...);
        river_seat_v1_focus_window(seat, interacted->obj);
        seat->focused = seat->interacted;
    }
    // ← filha cai aqui: NENHUM ramo. Interação descartada silenciosamente.
}
seat->interacted = NULL;
```
**A interação com a filha não casa em nenhum ramo.** Nada acontece: `focus_dirty` não é setado, o foco não muda. O river mantém o foco de teclado onde estava (na pai/última focada). Por isso o **campo de texto não recebe foco**.

> Por que botões "funcionam" e campo de texto "não": o clique do pointer é entregue pelo river ao surface sob o cursor independentemente do foco de **teclado**. Um botão reage a press/release de **ponteiro** → "funciona". Um campo de texto precisa de **foco de teclado** (`wl_keyboard.enter`) para receber digitação e mostrar cursor — e é exatamente o foco de teclado que o WM nunca concede à filha. (A confirmar com o agy: ver §7.)

### 4b. Caminho do Win+Tab (TOGGLE_TARGET → `focus_target_on_seats`)

Win+Tab alterna `target_index` entre 0 e 1 (`ACTION_TOGGLE_TARGET`, wm-input.c:497-503) e marca `focus_dirty`. No fim do manage cycle, `focus_target_on_seats()` (wm-layout.c:502-530) decide o foco:

```c
struct Window *target = target_window();      // raiz (MAIN ou DECK)
struct Window *focus = target;
if (target != NULL) {
    // varre filhas; se a RAIZ da filha == target, foca a filha
    wl_list_for_each(w, &wm.windows, link) {
        if (w->parent == NULL || !window_is_really_visible(w)) continue;
        struct Window *root = w->parent;
        while (root->parent != NULL && ++depth < 32) root = root->parent;
        if (root == target) focus = w;        // ← redireciona p/ a filha
    }
}
```

Ou seja: **existe** lógica para redirecionar o foco do target para a filha. Por que falha então?

A condição-chave é `window_is_really_visible(w)` (wm-layout.c:1172-1186): para uma filha, sobe até a raiz e só retorna `true` se a raiz for `view->main_win` / `view->deck_win` (ou `view->target` no modo single). **A `LayoutView` é montada na hora** (`layout_view_init` dentro de `window_is_really_visible`), refletindo o estado **corrente** de `target_index`.

Cenário do bug (2 janelas, A=pai-com-filha no slot, B=outra):
1. Foco em A → filha de A focada (redirect funciona). OK.
2. Win+Tab → `target_index` aponta para B. Foco vai para B. OK.
3. Win+Tab de volta → `target_index` aponta para A de novo. `target = A`. O laço encontra a filha de A, `root == A == target`. **Deveria** redirecionar `focus = filha`.

**Reexame de `window_is_really_visible` (refuta a hipótese ingênua):** `layout_view_init`
(wm-layout.c:185-203) define `main_win` = 1ª janela `parent==NULL && !floating && !minimized`
na ordem de `wm.windows`, e `deck_win` = a 2ª — **independentemente de `target_index`**.
No caso 2-janelas, A e B são as duas raízes nos dois primeiros slots, logo são
`main_win` e `deck_win`. `window_is_really_visible(filha de A)` sobe até A, que **é**
`main_win`/`deck_win` → retorna `true`. Portanto o redirect **deveria** focar a filha
de A no passo 3. **A condição de visibilidade NÃO é a causa.** (Esta era a H1 inicial;
fica **refutada** pela leitura de `layout_view_init`.)

**Hipóteses remanescentes (a discriminar com instrumentação/agy):**
- (H-A) **Toggle não marca a filha / não há nada a redesenhar.** `ACTION_TOGGLE_TARGET`
  só muda `target_index` e seta `focus_dirty`. O foco é reaplicado em
  `focus_target_on_seats`, **mas** essa função só roda se `layout_changed || focus_dirty`
  (wm-handlers.c:833). `focus_dirty` está true → roda. Então roda; o redirect calcula
  `focus = filha`. **Se Win+Tab realmente leva à pai, o redirect teria que estar
  retornando a pai** — o que, pela análise acima, não deveria ocorrer. ⇒ Possível que o
  sintoma "Win+Tab vai pra pai" seja na verdade **o mesmo do clique** mascarado: a filha
  recebe `focus_window` por um instante, mas algum evento subsequente (interação de
  ponteiro residual, `focus_none` da layer-shell em wm-handlers.c:937-940, ou um segundo
  manage cycle) reverte. **Precisa de trace temporal.**
- (H-B) **`seat->focused` x early-continue:** a guarda em wm-layout.c:519
  (`if (focused->floating && ... && !focus_dirty) continue`) **não** dispara (filha não é
  floating, focus_dirty=true). Descartada salvo evidência.
- (H-C) **Win+Tab pode já funcionar** e o relato principal ser o **clique** (§4a, este sim
  comprovadamente quebrado e corroborado pela evidência ao vivo de `text-input without
  focus`). O usuário descreve os dois juntos; é plausível que o caminho 4b funcione em
  alguns layouts e o 4a seja o defeito dominante. **A instrumentação dirá** se Win+Tab
  emite `focus_window(filha)` ou `focus_window(pai)` no passo 3.

**Conclusão honesta deste ponto:** o caminho do **clique (4a) é a causa provada** (cai fora
de todos os ramos de `seat_manage` + evidência de `text-input without focus`). O caminho do
**Win+Tab (4b) tem um redirect que, na leitura estática, deveria funcionar** — então ou há um
fator dinâmico (H-A: reversão pós-foco) ou ele já funciona e o relato mistura os dois (H-C).
Isto é precisamente o que o agy deve ajudar a discriminar (§7.1).

### 4c. Por que os dois caminhos divergem de design

O caminho 4a (clique) **não tem** o redirect-para-filha que o 4b (Win+Tab) tem. São duas implementações independentes de "quem focar", e só uma conhece filhas — e mesmo essa depende de uma condição de visibilidade frágil. **A causa raiz comum é: o foco do WM é expresso em termos de "target raiz", e o tratamento de filha é um patch parcial colado só no caminho declarativo (4b), ausente no caminho de interação (4a).**

## 5. Lado servidor (river) — inocentado

- `river/Seat.zig:472-484`: no `sendWindowInteraction`, o river envia a janela **exata** sob o cursor (`seat.wm_scheduled.interaction.window`), incluindo filhas. Logo o WM recebe a filha correta — o problema é o WM descartá-la (§4a).
- `river/Seat.zig:591-596` (`focus_window` request): aceita **qualquer** janela com userdata; **não** exige que esteja no layout. Portanto o WM **pode** focar uma filha diretamente via `river_seat_v1_focus_window(seat, child->obj)`. O servidor coopera; falta o WM exercer.

Conclusão: **bug 100% no maindeck-wm**, em dois pontos (`seat_manage` e `focus_target_on_seats`). Não requer mudança no protocolo nem no compositor.

## 6. Dropdowns "somem atrás" (hipótese separada, NÃO confirmada)

Dropdowns/popups (menus de combo) em Wayland são `xdg_popup` ancorados ao surface pai; normalmente **não** geram um toplevel river separado (não viram `struct Window`). Se "somem atrás", a causa provável é **stacking/posicionamento**, não foco:
- A ordem de empilhamento é dada por `wm_place_top` + a ordem de `window_render_layout` em `wm_handle_render_start` (wm-handlers.c:864-887): tiled → place_top(target) → children → floating. Se a Steam é tratada como filha/floating e o popup pertence a um surface que fica **abaixo** do alvo recolocado por `wm_place_top`, o popup pode ser ocluído.
- **Marcado como item à parte.** Só investigar a fundo após resolver o foco; pode ser o mesmo root (a filha não está "no topo" porque não é o foco/target) ou um bug de stacking de popup independente. Não bloquear a correção de foco por isto.

## 7. Pontos a validar com o agy (segunda opinião)

1. **(crítico)** Discriminar H-A vs H-C no caminho Win+Tab: instrumentar `focus_target_on_seats` para logar, no passo 3 do cenário, `target`, e para cada filha: `root`, `root==target`, `window_is_really_visible(w)`, e o `focus` final efetivamente passado a `river_seat_v1_focus_window`. Se logar `focus=<filha>` mas o foco visível for a pai ⇒ reversão dinâmica (H-A); se logar `focus=<pai>` ⇒ há erro na leitura estática a revisar; se Win+Tab focar a filha corretamente ⇒ H-C (o bug é só o clique). A evidência ao vivo de `text-input without focus` já **prova** o caminho do clique (4a).
2. Confirmar a explicação pointer-vs-keyboard (§4a nota): botões reagem a ponteiro, campo de texto exige `wl_keyboard.enter`. Validar que o river só manda `keyboard enter` para a janela que recebeu `focus_window`.
3. O fix deve ser **um só** caminho de foco que conheça filhas (unificar 4a e 4b), ou dois patches simétricos? (preferência de design a debater).
4. Há risco de a filha receber foco e **roubar** o target/slot? (a filha não deve virar target — não tem índice; só deve receber o foco de teclado enquanto a raiz dela é o alvo).
5. Reentrância: focar a filha em `seat_manage` (dentro do manage cycle) vs. deixar para `focus_target_on_seats` no fim — qual respeita melhor a regra "focus_window só dentro da manage sequence" (types.h:182 menciona `pending_float_focus` por isso).
6. Modais de profundidade > 1 (filha de filha): o laço de `focus_target_on_seats` já sobe N níveis (até 32). O caminho do clique precisa do mesmo cuidado.

## 8. Resumo executivo

- **O quê:** foco de teclado nunca vai para janelas filhas em dois cenários (clique / Win+Tab de volta).
- **Onde:** `maindeck-wm`, `seat_manage` (wm-input.c:632-646) e `focus_target_on_seats` (wm-layout.c:502-530).
- **Por quê:** filhas não têm índice MainDeck; o caminho do clique não trata filha (cai fora de todos os ramos) e o caminho do Win+Tab tem um redirect-para-filha que depende de uma checagem de visibilidade (`window_is_really_visible`) que aparentemente falha ao voltar o target para a pai (H1).
- **Servidor:** inocentado; aceita focar filha diretamente.
- **Correção (direção, a detalhar no plano):** dar à filha um caminho de foco explícito — quando a interação/target recai sobre uma raiz que tem filha visível, focar a filha (a mais relevante/topo) via `river_seat_v1_focus_window`, em ambos os caminhos, idealmente unificados.

---

## 9. Conclusões do debate com o agy (2 rodadas, 2026-06-24)

Debate Claude ↔ agy (gatilho #9). Confiança final do agy: 95% (rodada 1) → 100% (rodada 2).
Cada veredicto abaixo foi **verificado independentemente por Claude** no código (não repassado cru).

**Confirmado por ambos (causa raiz):**
- §4a (clique) é a **causa PROVADA**: filha (`window_index==-1`, `floating==false`) não casa nenhum ramo de `seat_manage` (wm-input.c:632-645) → interação descartada. Corroborado pela evidência ao vivo `wlr_text_input_v3.c:190: Text input commit received without focus`.
- §3: as três funções de índice ignoram filhas (wm-layout.c:72-79, 89-98, 132-141). Filha não tem índice MainDeck.
- §5: river **inocentado** — envia a interação da janela exata (Seat.zig:472-484) e aceita `focus_window` de qualquer janela com userdata (Seat.zig:591-596). Bug 100% client-side.

**Resolvido — Win+Tab (4b) é H-C, não H-A:**
- `layout_view_init` (wm-layout.c:185-203) define `main_win`/`deck_win` como os 2 primeiros slots da **lista** (independente de `target_index`). No caso 2-janelas, `window_is_really_visible(filha)` retorna `true` e o redirect **deveria** focar a filha. **A hipótese ingênua "visibilidade é a causa" (H1) está REFUTADA.**
- Veredicto convergente: **Win+Tab funciona na análise estática (H-C)**. O sintoma percebido ("Win+Tab vai pra pai") é o **clique quebrado mascarando**: como o clique nunca focou a filha, o usuário percebe o conjunto como quebrado. **Não há evidência de reversão dinâmica (H-A).**

**Riscos levantados pelo agy e seus veredictos (verificados por Claude):**
- **#2 (foco da filha com índice -1 quebraria invariantes) → REFUTADO.** Listei todos os usos de `seat->focused` (wm-handlers.c:553,787; wm-layout.c:306,519,528; wm-input.c:472,486-487,643): nenhum o passa a `window_index()` nem assume índice ≥0. O único `window_index` sobre algo focado é `focused_before = target_window()` (wm-layout.c:694), que já é a raiz e trata -1. Sem alvo concreto. **agy concordou (rodada 2).**
- **#3 (early-continue da linha 519 não cobre filha → `focus_window` repetido a cada cycle com `layout_changed`) → VÁLIDO.** A filha não é floating, então a guarda `seat->focused->floating && !focus_dirty` (wm-layout.c:519) não a protege de re-foco redundante. O plano deve evitar re-emitir foco idêntico.
- **#4 / PONTO B (ordem: `pending_float_focus` reaplica foco em `manage_start` (wm-handlers.c:779-792) ANTES de `focus_target_on_seats`) → BURACO DE ORDEM REAL.** `focus_target_on_seats` só roda se `layout_changed || focus_dirty` (wm-handlers.c:833). **Crítico:** o redirect faz **busca linear** e foca "a filha que a busca resolver" — pode **sobrescrever uma interação específica** do usuário. ⇒ **Implicação de design forte: o caminho do clique deve registrar QUAL janela focar (a interagida), não delegar a um redirect que escolhe uma filha qualquer da raiz.**
- **Z-order de modais aninhados (profundidade >1):** filhas vão para a cauda (wm-handlers.c:425); o laço linear de `focus_target_on_seats` foca a última da lista cuja raiz==target. Com modais empilhados, pode focar o modal errado. O plano deve definir "filha mais relevante" (topo/mais recente).

**Escopo adicional aceito (PONTO A — CLOSE_TARGET):**
- `ACTION_CLOSE_TARGET` (wm-input.c:484-496): com a filha focada, a guarda `focused->floating` falha (filha não é floating) → cai em `target_window()` e **fecharia a PAI**. É bug latente que a correção de foco vai **expor** (hoje a filha nunca tem foco). **Decisão (ambos):** estender a guarda para `focused->floating || focused->parent != NULL` → fecha a filha focada. Órfãs já tratadas em `window_destroy_closed` (wm-handlers.c:562-569).

**Direção de correção consolidada (entra no plano):**
1. **Unificar** os dois caminhos de foco num fluxo que conhece filhas.
2. Caminho do **clique**: ao interagir com uma filha, subir à **raiz**, setar `wm.target_index` para essa raiz, marcar `focus_dirty=true`, e **registrar a janela interagida exata** como alvo de foco (não deixar o redirect linear escolher) — análogo ao `pending_float_focus` existente.
3. **CLOSE_TARGET**: estender guarda para fechar filha focada.
4. Evitar re-foco redundante (não re-emitir `focus_window` se já é o foco corrente).
5. Definir "filha mais relevante" para modais aninhados (topo da pilha).

**Pendências de baixa prioridade (fora do escopo imediato):**
- §6 dropdowns "somem atrás" (Steam): hipótese de **stacking de `xdg_popup`**, não foco. Investigar após o foco.
- Confirmação empírica de timing de concorrência do river (CAVEAT do agy) — não bloqueante; a ordem `manage_start → focus_target_on_seats (no fim)` garante última palavra quando o fix setar `focus_dirty`.
