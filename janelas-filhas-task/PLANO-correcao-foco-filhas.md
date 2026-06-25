# Plano de correção: foco em janelas filhas (child/dialog)

Data: 2026-06-24
Base: `DIAGNOSTICO-foco-filhas.md` (§9 — conclusões do debate, 2 rodadas com agy).
Alvo: `maindeck-wm`. Sem mudança de protocolo nem no compositor (river inocentado).

## 0. Princípio da correção

O foco do WM hoje é expresso só em termos de "target raiz" (MAIN/DECK). Filhas
(`parent!=NULL`, `floating==false`) não têm índice e só recebem foco por um redirect
linear frágil presente apenas no caminho declarativo (Win+Tab). O caminho do clique não
trata filha → interação descartada (causa raiz provada).

**Correção:** unificar o foco num único conceito — "**janela de foco efetiva**" — que,
dado o target raiz, escolhe a **filha mais relevante** (topo da pilha de modais) se houver,
senão a própria raiz. E dar ao **clique** um caminho explícito que registre **qual** janela
focar (a interagida), em vez de depender do redirect linear (que pode focar a filha errada —
buraco de ordem #4 do debate).

Reaproveita o que já existe: `root_window()` (wm-layout.c:736, sobe à raiz com guarda
depth<32) e o padrão `pending_float_focus` (foco diferido aplicado no manage cycle).

## 1. Helper: "filha mais relevante de uma raiz" (NOVA função)

**Arquivo:** `wm-layout.c` (perto de `root_window`, ~linha 736). **Header:** declarar em `wm-layout.h`.

```c
// Dada uma janela RAIZ (parent==NULL), retorna a filha visível mais relevante para
// receber foco — a ÚLTIMA na ordem de wm.windows cuja raiz é `root` e que está
// realmente visível. "Última" = topo da pilha de modais (children vão para a cauda em
// window_handle_parent, wm-handlers.c:425; o mais recente fica por último). Se não há
// filha visível, retorna a própria raiz. NULL-safe.
struct Window *md_effective_focus_window(struct Window *root) {
    if (root == NULL) return NULL;
    struct Window *focus = root;
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->parent == NULL || !window_is_really_visible(w)) continue;
        if (root_window(w) == root) focus = w; // último match vence = topo da pilha
    }
    return focus;
}
```

Notas:
- Mantém a semântica "último match vence" já usada hoje em `focus_target_on_seats`, mas
  isolada e nomeada. Resolve modais aninhados de forma **definida** (topo = mais recente),
  endereçando o risco de Z-order do debate.
- `window_is_really_visible(w)` monta `LayoutView` próprio — custo aceitável (já era o
  comportamento atual). Não muda performance perceptível (chamada só em eventos de foco).

## 2. Refatorar `focus_target_on_seats` para usar o helper

**Arquivo:** `wm-layout.c:502-530`. Trocar o bloco de redirect inline pelo helper.

ANTES (502-515):
```c
struct Window *target = target_window();
struct Window *focus = target;
if (target != NULL) {
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->parent == NULL || !window_is_really_visible(w)) continue;
        struct Window *root = w->parent;
        int depth = 0;
        while (root->parent != NULL && ++depth < 32) root = root->parent;
        if (root == target) focus = w;
    }
}
```
DEPOIS:
```c
struct Window *target = target_window();
struct Window *focus = md_effective_focus_window(target);
```

Comportamento idêntico para o caso geral (mantém Win+Tab funcionando — H-C), mas agora
via função única. **Sem mudança de comportamento aqui** — é refator para unificar. O loop
inline subia o parent manualmente (sem `root_window`); o helper usa `root_window` (mesma
guarda depth<32). Equivalente.

> **Nota:** este item 2 mostra só a primeira linha da refatoração (redirect → helper). A
> versão **canônica e final** de `focus_target_on_seats` (com float > child > redirect +
> guarda) está no **§3c** — é essa que deve ser escrita. Não aplicar item 2 e §3c como dois
> patches; §3c já é o estado final da função.

## 3. Caminho do CLIQUE: registrar foco explícito da janela interagida (CORE FIX)

### 3a. Novo campo de estado

**Arquivo:** `types.h`, `struct WindowManager` (perto de `pending_float_focus`, ~linha 182):
```c
// Janela (tiled/child) a focar no próximo manage cycle, registrada por uma interação
// de ponteiro que recaiu sobre uma FILHA. Diferente de pending_float_focus (que é só
// para flutuantes e força foco direto). Aqui guardamos a filha EXATA clicada para não
// deixar o redirect linear escolher outra filha da mesma raiz (buraco de ordem do
// debate §9 #4). NULL = nada pendente.
struct Window *pending_child_focus;
```

### 3b. Tratar a filha em `seat_manage`

**Arquivo:** `wm-input.c:632-646`. O bloco de `seat->interacted` ganha um ramo para filha.

ANTES (632-645):
```c
if (seat->interacted != NULL) {
    int32_t idx = window_index(seat->interacted);
    if (idx == 0 || idx == 1) {
        if (idx != (int32_t)wm.target_index) {
            wm.target_index = (uint32_t)idx;
            wm.maximized = false;
        }
        wm.focus_dirty = true;
    } else if (seat->interacted->floating) {
        river_seat_v1_clear_focus(seat->obj);
        river_seat_v1_focus_window(seat->obj, seat->interacted->obj);
        seat->focused = seat->interacted;
    }
}
seat->interacted = NULL;
```
DEPOIS (inserir ramo `else if (parent != NULL)` ANTES do `else if (floating)`):
```c
if (seat->interacted != NULL) {
    int32_t idx = window_index(seat->interacted);
    if (idx == 0 || idx == 1) {
        if (idx != (int32_t)wm.target_index) {
            wm.target_index = (uint32_t)idx;
            wm.maximized = false;
        }
        wm.focus_dirty = true;
    } else if (seat->interacted->parent != NULL) {
        // Interação recaiu sobre uma FILHA (diálogo/modal). Dois casos pela RAIZ:
        struct Window *root = root_window(seat->interacted);
        if (root->floating) {
            // Raiz floating (ex.: satty/diálogo de config com sub-diálogo): a raiz não
            // tem índice MainDeck. Foca a filha DIRETO, sem mexer no target nem em
            // focus_dirty — simétrico ao tratamento de floating abaixo. (Se em vez disso
            // registrássemos pending_child_focus, focus_target_on_seats não casaria
            // root_window(pc)==target (target é tiled) e re-focaria o tiled, roubando o
            // foco da app floating — regressão apontada no debate do plano §11 #1.)
            river_seat_v1_clear_focus(seat->obj);
            river_seat_v1_focus_window(seat->obj, seat->interacted->obj);
            seat->focused = seat->interacted;
        } else {
            // Raiz tiled: sobe à raiz, torna-a o target (para a filha ser "do alvo") e
            // registra a filha EXATA como foco pendente — focus_target_on_seats aplica
            // no fim do cycle (última palavra). Não focamos aqui dentro do manage para
            // manter toda decisão de foco num lugar só (unificação) e respeitar a ordem.
            int32_t ridx = window_index(root);
            if (ridx == 0 || ridx == 1) {
                if (ridx != (int32_t)wm.target_index) {
                    wm.target_index = (uint32_t)ridx;
                    wm.maximized = false;
                }
            }
            wm.pending_child_focus = seat->interacted;
            wm.focus_dirty = true;
        }
    } else if (seat->interacted->floating) {
        river_seat_v1_clear_focus(seat->obj);
        river_seat_v1_focus_window(seat->obj, seat->interacted->obj);
        seat->focused = seat->interacted;
    }
}
seat->interacted = NULL;
```
- `root_window` já está declarado/visível? Confirmar `wm-layout.h` exporta. Se `root_window`
  for `static` em wm-layout.c (é — linha 736 `static`), **precisa** ser exportado:
  remover `static` e declarar em `wm-layout.h`. (Item 6.)

### 3c. `focus_target_on_seats` como ÁRBITRO ÚNICO de foco (Opção B — debate §11 R2)

**Arquivo:** `wm-layout.c:502-530`. Esta função passa a ser o **único lugar** que aplica
foco no manage cycle, com precedência explícita:
**`pending_float_focus` > `pending_child_focus` > `md_effective_focus_window(target)`**.
Isso elimina por construção a corrida float-vs-child (a aplicação do float **sai** do
manage_start — ver §4b) e cumpre o objetivo de unificação do §0.

DEPOIS (combina itens 2, 3c, 4b, 5):
```c
void focus_target_on_seats(void) {
    clamp_target();
    struct Window *target = target_window();
    struct Window *focus = md_effective_focus_window(target);

    if (wm.pending_float_focus != NULL) {
        // Maior precedência: flutuante recém-exibível pedindo foco (config-list,
        // satty, primeira dimensão de uma floating). A aplicação migrou do
        // manage_start para cá (§4b) p/ não competir com pending_child_focus.
        struct Window *pf = wm.pending_float_focus;
        wm.pending_float_focus = NULL;
        if (!pf->closed && pf->floating) {
            focus = pf;
            wm.pending_child_focus = NULL; // a nova flutuante anula a interação pendente na filha
        }
    } else if (wm.pending_child_focus != NULL) {
        // Interação de ponteiro registrou uma filha EXATA (raiz tiled): vence o redirect
        // linear (que poderia escolher outra filha da mesma raiz). Só honra se ainda
        // visível e pertencente ao target atual.
        struct Window *pc = wm.pending_child_focus;
        wm.pending_child_focus = NULL;
        if (!pc->closed && pc->parent != NULL &&
            window_is_really_visible(pc) && root_window(pc) == target) {
            focus = pc;
        }
    }

    struct Seat *seat;
    wl_list_for_each(seat, &wm.seats, link) {
        if (seat->removed) continue;
        if (seat->focused != NULL && seat->focused->floating && !seat->focused->closed && !wm.focus_dirty) {
            continue;
        }
        // (item 5) evita re-foco idêntico, MAS não quando focus_dirty — após focus_none
        // da layer-shell (wm-handlers.c:937-940) o foco REAL foi limpo e seat->focused
        // ficou obsoleto; re-emitir é necessário. focus_dirty cobre esse caso.
        if (seat->focused == focus && !wm.focus_dirty) continue;
        if (focus != NULL) {
            river_seat_v1_clear_focus(seat->obj);
            river_seat_v1_focus_window(seat->obj, focus->obj);
        } else {
            river_seat_v1_clear_focus(seat->obj);
        }
        seat->focused = focus;
    }
}
```

**Log opcional (recomendado p/ verificação headless):** ao aplicar `pending_child_focus`,
emitir `LOG_EVENT("child focus applied: \"%s\"", pc->title?...)` — confirma o fix sem GUI.

- **Importante:** `pending_child_focus` precisa ser zerado em `window_destroy_closed`
  (wm-handlers.c:548-594), junto com `pending_float_focus` (linha 559-561), para não
  deixar dangling pointer. (Item 4a.)

## 4. Limpeza de dangling + precedência float vs child

### 4a. Zerar `pending_child_focus` no destroy (dangling)

**Arquivo:** `wm-handlers.c`, `window_destroy_closed` (~559-561, ao lado de `pending_float_focus`):
```c
if (wm.pending_float_focus == window) {
    wm.pending_float_focus = NULL;
}
if (wm.pending_child_focus == window) {   // NOVO
    wm.pending_child_focus = NULL;
}
```
`wm` é zero-inicializado (`struct WindowManager wm;` em wm-ipc.c:41, sem inicializador → BSS),
logo `pending_child_focus` começa NULL sem init explícito. (Mesma razão pela qual
`pending_float_focus` não precisa de init.)

### 4b. Mover aplicação do `pending_float_focus` para `focus_target_on_seats` (Opção B — DECIDIDO)

Decisão do debate §11 R2 (agy + verificação independente de Claude): **Opção B** (unificação
real). A aplicação do float **sai** do manage_start e vira a maior precedência dentro de
`focus_target_on_seats` (§3c). Isso elimina por construção a corrida float-vs-child — não há
mais "aplicar em dois lugares". Precedência: **float > child > redirect**.

**Verificação que sustenta a segurança (confirmada por Claude lendo wm-handlers.c:758-842 e
wm-layout.c):** nenhuma rotina entre a antiga aplicação (:779) e a chamada de
`focus_target_on_seats` (:834) **lê** `seat->focused` de modo que dependa do float ter sido
focado antes:
- `apply_pending_taskbar_activation` (:777) → `activate_window_from_taskbar` apenas **escreve**
  `seat->focused` (wm-layout.c:306, ramo floating de taskbar); não lê.
- `apply_pending_window_action` (:778) → não referencia `seat->focused` (verificado).
- `clamp_target` (:793), `layout_view_init`, `compute_layout_signature`, `window_manage_layout`
  → operam sobre lista/índices/geometria, não sobre `seat->focused`.
Logo, atrasar a aplicação do foco do float de :779 para :834 é seguro.

**Mudança 1 — remover o bloco antigo** em `wm_handle_manage_start` (wm-handlers.c:779-792):
```c
// REMOVER todo este bloco (a aplicação migra p/ focus_target_on_seats, §3c):
//   if (wm.pending_float_focus != NULL) { ... aplica foco ... }
```
(O `pending_float_focus` continua sendo **setado** pelos seus setters atuais — wm-handlers.c:240
e :355 — só a *aplicação* muda de lugar.)

**Mudança 2 — estender o gate de execução** de `focus_target_on_seats` (wm-handlers.c:833).
Hoje o float era aplicado incondicionalmente; agora `focus_target_on_seats` precisa rodar
sempre que houver float pendente (sem precisar sujar `focus_dirty` nos setters):
```c
// ANTES:
if (layout_changed || wm.focus_dirty) {
// DEPOIS:
if (layout_changed || wm.focus_dirty || wm.pending_float_focus != NULL) {
```
Dentro de `focus_target_on_seats`, o bloco de maior precedência (§3c) consome
`pending_float_focus` (zera + foca + anula `pending_child_focus`). Como o gate agora inclui
`pending_float_focus != NULL`, a função **sempre** roda quando há float pendente — equivalente
ao comportamento incondicional antigo, mas centralizado.

> **Observação (fora de escopo, sem regressão):** `activate_window_from_taskbar` (wm-layout.c:296-311,
> ramo floating de taskbar-activate) ainda aplica foco direto em :777, e `focus_target_on_seats`
> em :834 poderia sobrescrevê-lo se rodar no mesmo cycle. **Isso já é o comportamento atual**
> (coexistência pré-existente, não introduzida por este plano). NÃO tratar aqui — fora do escopo
> de foco-em-filhas. Registrado para eventual follow-up.

## 5. Evitar re-foco redundante (risco #3 do debate)

Já incluído no item 3c: a linha `if (seat->focused == focus) continue;` impede re-emitir
`clear_focus`+`focus_window` quando o foco já é o desejado. Isso neutraliza o efeito de o
early-continue da linha 519 não cobrir filhas (filha re-focada a cada `layout_changed`).

**Cuidado (resolvido no debate do plano §11 #3):** a guarda muda o comportamento atual
(hoje re-emite sempre). Bug concreto descoberto: quando a layer-shell (launcher/barra) some,
o river emite `focus_none`, **limpando o foco REAL do teclado**, e o WM seta `focus_dirty`
(wm-handlers.c:937-940) **sem** atualizar `seat->focused` — que continua apontando a janela
anterior. Se o foco desejado após fechar o launcher for **essa mesma janela**, a guarda
`seat->focused == focus` pularia o re-foco → **sessão inteira fica sem foco de teclado**.
**Correção adotada:** `if (seat->focused == focus && !wm.focus_dirty) continue;`. Como
`focus_none` seta `focus_dirty`, a guarda não pula nesse caso e o foco é reemitido. A guarda
fica com a **mesma condição** `&& !focus_dirty` da guarda de floating (linha 519) —
coerente: ambas só "preservam" o estado quando não há sujeira de foco pendente.

## 6. Exportar `root_window`

**Arquivo:** `wm-layout.c:736` — remover `static`. **`wm-layout.h`** — declarar:
```c
struct Window *root_window(struct Window *window);
struct Window *md_effective_focus_window(struct Window *root);
```

## 7. CLOSE_TARGET: fechar a filha focada (escopo aceito — PONTO A)

**Arquivo:** `wm-input.c:484-496` (`ACTION_CLOSE_TARGET`). Estender a guarda.

ANTES (486):
```c
if (seat->focused != NULL && seat->focused->floating && !seat->focused->closed) {
    river_window_v1_close(seat->focused->obj);
    break;
}
```
DEPOIS:
```c
if (seat->focused != NULL && !seat->focused->closed &&
    (seat->focused->floating || seat->focused->parent != NULL)) {
    river_window_v1_close(seat->focused->obj);
    break;
}
```
Fecha a filha focada (Win+Del/Alt+F4) em vez de cair no `target_window()` e fechar a pai.
Órfãs já tratadas em `window_destroy_closed` (wm-handlers.c:562-569: limpa `parent` das
janelas cujo `parent==window`).

## 8. FORA DE ESCOPO (não implementar agora)

- **Dropdowns "somem atrás" (Steam):** hipótese de stacking de `xdg_popup` (§6 do
  diagnóstico), **não** foco. Investigar separadamente após validar o foco. NÃO mexer em
  `wm_handle_render_start`/`wm_place_top` neste plano.
- Qualquer refator de animação, layout de child (posicionamento/centralização), ou
  protocolo. Este plano é **só foco + o close adjacente**.

## 9. Build e verificação

- **Build:** meson (confirmado: `meson.build` + dir `build/` já configurado). Comando:
  `meson compile -C build`. Delegar ao agy (gatilho #1 — output verboso/warnings nível 3).
  O `build/` já tem o "máximo local" (-O3 -march=native) aplicado via `meson configure`; para
  validar a correção basta recompilar (não precisa reconfigurar). Trazer só erros/warnings
  destilados.
- **Verificação comportamental (casos de aceitação):**
  1. Abrir app com diálogo modal filho (ex.: WPS "Salvar como", ou um zenity/kdialog).
     Clicar num **campo de texto** da filha → digitação aparece (foco de teclado concedido).
     **Critério:** parar de ver `wlr_text_input_v3.c:190: ... without focus` no log ao
     digitar na filha.
  2. Com a filha aberta e focada, mover foco para outra janela (Win+Tab) e voltar (Win+Tab)
     → foco volta para a **filha**, não para a pai.
  3. Botões/seleções da filha continuam funcionando (não regrediram).
  4. Win+Del / Alt+F4 com a filha focada → fecha a **filha**, a pai permanece.
  5. Modais aninhados (se reproduzível): foco vai para o **topo** da pilha.
  6. Sem filha (caso comum 2 janelas tiled): Win+Tab e clique continuam idênticos ao atual
     (sem regressão no caminho raiz).
- **Não fechar a sessão viva** para testar; usar uma sessão/instância de teste ou validar
  via log. Instrumentação `LOG_EVENT` nos novos ramos (interação-filha, pending_child_focus
  aplicado) ajuda a confirmar sem GUI.

## 10. Resumo dos pontos de edição

| # | Arquivo | Local | Mudança |
|---|---------|-------|---------|
| 1 | wm-layout.c + .h | ~736 / header | NOVA `md_effective_focus_window`; tornar `root_window` não-`static` e declarar ambas no header |
| 2 | wm-layout.c | 502-515 | redirect inline → `md_effective_focus_window(target)` (embutido no §3c) |
| 3a| types.h | ~182 (struct WindowManager) | NOVO campo `struct Window *pending_child_focus;` |
| 3b| wm-input.c | 632-646 (seat_manage) | ramo `else if (interacted->parent != NULL)`: raiz floating → foca filha direto; raiz tiled → target=raiz + `pending_child_focus`=filha + `focus_dirty` |
| 3c| wm-layout.c | 502-530 (focus_target_on_seats) | árbitro único: precedência **float > child > redirect**; guarda re-foco `&& !focus_dirty` |
| 4a| wm-handlers.c | ~559 (window_destroy_closed) | zerar `pending_child_focus` no destroy (anti-dangling) |
| 4b| wm-handlers.c | 779-792 + 833 | **remover** bloco que aplica `pending_float_focus`; **estender gate**: `if (layout_changed \|\| focus_dirty \|\| pending_float_focus != NULL)` |
| 7 | wm-input.c | 484-496 (ACTION_CLOSE_TARGET) | guarda `(focused->floating \|\| focused->parent != NULL)` → fecha filha focada |

(Itens 5 e 6 do texto estão embutidos: 5 = guarda re-foco dentro do §3c; 6 = export de `root_window` dentro do §1.)

**Arquivos tocados (5):** `wm-layout.c`, `wm-layout.h`, `types.h`, `wm-input.c`, `wm-handlers.c`.
Mudança coesa, centrada em foco; **sem** tocar render/animação/protocolo/posicionamento de child.

## 11. Registro do debate do plano (2 rodadas com agy, 2026-06-24)

Confiança final agy: 100% (R1) / 100% (R2). Cada veredicto **verificado por Claude** no código.

**Rodada 1 — 3 problemas reais encontrados pelo agy (todos incorporados):**
1. **(Alta) Filha de raiz floating** → `window_index(root)==-1`; registrar `pending_child_focus`
   roubaria foco da app floating. **Fix:** ramo §3b detecta `root->floating` e foca a filha
   direto. *(Claude confirmou: filha PODE ter raiz floating — `window_handle_parent` só zera
   `floating` na filha, não no pai; pai pode ter sido designado floating antes.)*
2. **(Média) Corrida `pending_float_focus` vs `pending_child_focus`** no mesmo cycle. *(Ao
   incorporar, Claude descobriu que a correção mínima do agy era insuficiente — `focus_dirty`
   mantinha a sobrescrita. Levado à R2.)*
3. **(Alta) Perda de foco após `focus_none` da layer-shell** com a guarda de re-foco. **Fix:**
   guarda vira `&& !wm.focus_dirty`. *(Claude confirmou: `focus_none` seta `focus_dirty` sem
   tocar `seat->focused` — wm-handlers.c:937-940.)*
   Itens checados e marcados **OK** pelo agy: refator do redirect (sem off-by-one), filha/raiz
   minimizada (coberto por `window_is_really_visible`), modal aninhado profundidade>1.

**Rodada 2 — decisão da corrida float-vs-child (Problema #2 refinado):**
- Claude apresentou Opções A/B/C. agy **refutou C** (clique em modal do meio seria sobrescrito
  pelo redirect; corrida com float persiste) e **A** (acoplamento via flags temporárias).
- **Escolhida Opção B** (unificação): aplicação do float **migra** para
  `focus_target_on_seats` no topo da precedência; gate estendido com `pending_float_focus != NULL`.
- agy confirmou (e **Claude re-verificou** lendo wm-handlers.c:758-842 + wm-layout.c) que
  **nada entre :779 e :834 lê `seat->focused`** → mover é seguro. `apply_pending_window_action`
  não toca `focused`; `activate_window_from_taskbar` só escreve (:306); demais rotinas operam
  sobre lista/geometria.

**Conclusão:** plano final = Opção B. Coeso, verificado nos dois lados, riscos endereçados.
