## Plano: foco_janela_swap

### Resumo
Inserir `wm.target_index = wm.target_index === 0 ? 1 : 0;` em `actionSwapMainDeck` de 10 protótipos, imediatamente após o swap de `wm.windows[0]`/`wm.windows[1]` e antes de qualquer chamada de layout/animação/render. Tarefa repetitiva; risco está na posição exata e em escopos de `setTimeout`.

### Arquivos
docs/prototype-anim-{p2, p3, p3.1, p5, p5.1, p7, p8, p9, p10, p11}.html — função `actionSwapMainDeck`.

### Procedimento por arquivo (repetir 10x)
1. Localizar `function actionSwapMainDeck` (ou `actionSwapMainDeck = ` / `actionSwapMainDeck()` em objeto).
2. Identificar o bloco de swap do array. Formas esperadas (qualquer uma):
   - `const tmp = wm.windows[0]; wm.windows[0] = wm.windows[1]; wm.windows[1] = tmp;`
   - `[wm.windows[0], wm.windows[1]] = [wm.windows[1], wm.windows[0]];`
   - `wm.windows.reverse();` (se só houver 2 janelas relevantes).
3. **Checar idempotência**: se a linha `wm.target_index = wm.target_index === 0 ? 1 : 0;` já existir dentro dessa função, NÃO modificar (o brief diz "em que essa linha está ausente"). Pular o arquivo e registrar.
4. Inserir a linha de correção na PRIMEIRA linha logo após a última instrução do swap do array, mesma indentação, e ANTES de:
   - chamadas de layout (ex.: `layout*()`, `applyLayout`, `relayout`),
   - chamadas de animação/render (`animate*`, `render*`, `requestAnimationFrame`),
   - qualquer `setTimeout(...)`.
5. **Escopo de timer**: se o swap do array estiver DENTRO do callback de um `setTimeout`, a linha deve entrar dentro do mesmo callback, junto ao swap — NUNCA fora do timer. Se houver dois pontos (swap fora e re-render dentro de timer), a atribuição acompanha o swap do array.
6. Não alterar mais nada (sem reformatar, sem mover outras linhas).

### Contrato da edição
- Texto inserido exato: `        wm.target_index = wm.target_index === 0 ? 1 : 0;` (ajustar indentação ao bloco; preferir tabs/espaços já usados no arquivo).
- Posição: `swap(wm.windows)` → **linha nova** → `layout/animate/setTimeout`.

### Critérios de aceitação
1. Cada um dos 10 arquivos contém exatamente UMA ocorrência da linha dentro de `actionSwapMainDeck` (a menos que já existisse — então mantém a original e zero duplicação).
2. A linha aparece após o swap do array e antes da primeira chamada de layout/animação/timer no mesmo escopo.
3. Indentação consistente com o bloco circundante; nenhuma outra linha alterada (diff mínimo: +1 linha por arquivo, salvo arquivos já corrigidos).
4. HTML continua válido; sem erro de sintaxe JS (`<script>` ainda parseável).

### Validação (gate estático)
- `grep -n "wm.target_index = wm.target_index === 0 ? 1 : 0;" docs/prototype-anim-*.html` → confirmar 1 hit por arquivo-alvo.
- Para cada hit, inspecionar visualmente as ~3 linhas anteriores (swap presente) e seguintes (layout/anim/timer) confirmando ordem.
- `git diff --stat` deve mostrar ~+1 linha por arquivo modificado, sem remoções inesperadas.

### Riscos
1. **Variação de assinatura/forma do swap** entre protótipos (reverse vs tmp vs destructuring) — não assumir um padrão único; localizar o swap real em cada arquivo antes de inserir.
2. **Múltiplos swaps ou swap dentro de setTimeout** — inserir no escopo correto; errar o escopo quebra a sincronização do foco (critério de aceitação falha silenciosamente).
3. **Arquivo já corrigido** — inserir duplicaria o toggle, anulando o efeito (volta ao estado original). Sempre checar existência antes (passo 3).
4. **`wm` com nome/escopo diferente** (ex.: `this.wm`, `state.wm`) — usar o mesmo identificador do swap local, não hardcodar `wm` cegamente.