## Revisão do diff — `otimizacao-jogos-task/diff.patch`

Resultado geral: a direção está correta e o cuidado aparece (ex.: limpeza do cache em `window_maybe_destroy`). Há **1 risco de regressão real** que precisa de verificação/correção e alguns pontos menores. Não aprovo ainda — peço deliberação sobre os itens 1–3.

### Achados que pedem ação

**1. `wm_place_top` — cache de slot único pode deixar a janela-alvo atrás (wm-layout.c:444-450, wm-handlers.c:351)**
`wm_place_top` só chama `river_node_v1_place_top` se `node != wm.last_placed_top_node`. Isso assume o invariante "nada altera o z-order exceto `wm_place_top`". O destroy trata isso (wm-handlers.c:208), mas o **map de uma nova janela não**. Cenário concreto: janela-alvo A já está no topo (`last_placed == A`); uma nova janela é mapeada e o compositor a insere acima; no próximo `render_start`, se o passo só reexecutar `wm_place_top(target=A)`, ele é **pulado** por `last == A` e A permanece coberta — exatamente o que o `place_top` final deveria garantir.
- Correção concreta: zerar o cache no caminho de criação/map de janela — `wm.last_placed_top_node = NULL;` onde uma nova `struct Window` é registrada (equivalente ao que já é feito no destroy). Custo nulo, elimina o caso de borda.
- Pedido ao agy: confirmar se o loop de `wm_handle_render_start` chama `window_render_layout` (e portanto `wm_place_top`) para **todas** as janelas em todo frame; se sim, o risco se restringe ao caso "alvo é a única/recém-criada janela", mas a correção acima cobre ambos.

**2. `is_warn` referenciado mas não definido no diff (wm-log.c:71)**
`if (is_warn || level[0] == 'E' || level[0] == 'C')` usa `is_warn`, que não aparece no patch — presume-se pré-existente em `md_log`. Se não existir, é erro de compilação.
- Ação: confirmar que `is_warn` já está declarado/atribuído na função (e que cobre `level[0]=='W'`, senão WARN deixa de dar flush). Trivial de checar, mas precisa ser confirmado antes do merge.

**3. `render_dirty` é campo morto (bar-state.h:103)**
`bool render_dirty;` é adicionado mas **nunca lido nem escrito** em nenhum arquivo do diff. Ou é resíduo de uma abordagem abandonada, ou falta a lógica que o usaria.
- Correção: remover o campo, ou esclarecer qual lógica ficou faltando.

### Pontos a confirmar (tradeoffs, não bugs)

**4. Full buffering perde INFO/DEBUG em crash (wm-log.c:21,72)**
Troca de `_IOLBF` → `_IOFBF` com flush só em WARN/ERROR/CRIT. É o ganho pretendido, mas num `SIGSEGV` sem handler os INFO desde o último flush somem. Se houver handler de sinal ou caminho de saída, vale chamar `log_close()`/`fflush` lá. Em `wm-ipc.c` o `log_close()` no `done:` (linha 207) cobre a saída limpa — ok; falta apenas o caminho anormal.

**5. Supressão de render assume a barra coberta (bar-main.c:273,324; bar-taskbar.c:14)**
Se a barra estiver em layer `overlay`/`top` que um cliente fullscreen **não** cobre, suprimir o render apenas congela o relógio/volume com a barra ainda visível. Confirmar que a barra do maindeck fica de fato encoberta pelo fullscreen (provável no contexto do WM, mas é o ponto que torna a otimização correta vs. visível-quebrada).

**6. Primeira render já suprimida no startup (bar-main.c:273)**
Se um jogo fullscreen já estiver ativo quando a barra inicia, `mgr_toplevel` → `bar_update_render_suppressed()` liga `render_suppressed` durante os roundtrips iniciais, e a render inicial é pulada → a surface layer-shell nunca anexa o primeiro buffer até o jogo fechar. Sugiro forçar a **primeira** render independentemente da supressão (a inicial em bar-main.c:273 não checar `render_suppressed`).

### Menores (opcionais)
- `bar_update_render_suppressed` é global sem protótipo em header e só usada em `bar-taskbar.c` → pode ser `static` (bar-taskbar.c:15).
- Linha em branco extra ao fim de `wm-log.c` (cosmético).

### Recomendação
Aprovo os blocos de logging-gating (`bar_verbose`/LOG_INFO) e o rastreio de `fullscreen` na toplevel — corretos. Bloqueio o merge nos itens **1, 2 e 3**: o item 1 é a única regressão funcional plausível (janela-alvo atrás em jogos), o 2 é risco de compilação, o 3 é código morto. Aguardo aprovação do agy para: aplicar `last_placed_top_node = NULL` no map de janela (item 1), confirmar `is_warn` (item 2) e remover `render_dirty` (item 3).