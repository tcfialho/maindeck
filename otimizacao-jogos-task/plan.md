# Plano de Implementação — Otimização Maindeck (Versão Final Aprovada)

## Item 4 — Logs Verbosos / I/O (executar primeiro)

**Arquivos:** módulo de logging (onde `MAINDECK_LOG` é lido).

- **Early-out por nível antes de formatar:** se nível < threshold, retornar sem `snprintf`/montagem de string.
- Default de execução passa a `info`/`warn` (não `debug`).
- Trocar buffering de linha por **block buffering** com flush em `warn+`; flush garantido em saída/sinal para não perder logs em caso de crash do WM.

## Item 1 — Redesenhos Fantasmas (barra invisível sob fullscreen)

**Arquivos:** `bar-state.h`, `bar-taskbar.c`, `bar-main.c`, `bar-render.c`.

- `bar-state.h`: adicionar `bool render_suppressed;` e `bool render_dirty;` ao estado da barra.
- `bar-taskbar.c`: no handler de `state` do `zwlr_foreign_toplevel_handle_v1`, passar a verificar a flag **`fullscreen` (enum valor `3`)** — confirmada como propagada corretamente por River/wlroots — combinada com `activated` para manter atualizada a condição **"existe toplevel ativado E fullscreen"** como fonte de verdade de `render_suppressed`.
- Substituir chamadas diretas a `bar_render()` por marcação `render_dirty = true`; o render real ocorre **uma vez por iteração** do event loop em `bar-main.c`.
- `bar-main.c`, fim da iteração: se `render_dirty && !render_suppressed` → renderiza e limpa dirty.
- **Invariante crítico:** na transição `suppressed: true → false`, **forçar render imediato** no mesmo ciclo. Cobrir os três gatilhos: `closed`, `state` com `minimized`, e `state` perdendo `activated`/`fullscreen`.

## Item 3 — Chamadas Redundantes de `place_top`

**Arquivos:** `wm-layout.c`, `wm-handlers.c`.

- Cachear por output o último alvo de `place_top` enviado (handle/identificador da view + assinatura de layout).
- Antes de emitir `place_top` e comandos de layout fullscreen por frame, comparar com o cache; **só enviar em mudança real**.
- Atualizar cache somente após envio bem-sucedido. Invalidar cache em `closed`/troca de output.
- Sem alteração de semântica River/wlroots — apenas supressão de reenvios idênticos.

---

## Planejamento Futuro

### Correção de Pareamento das Toplevels (Item 2)
As especificações técnicas e o planejamento detalhado para solucionar os problemas de race conditions e dessincronização de foco da barra de tarefas foram movidos para o arquivo [future-matching-plan.md](file:///home/tcfialho/Documents/poc/maindeck-wm/otimizacao-jogos-task/future-matching-plan.md) para análise e implementação em etapas posteriores.

---

## Ordem de execução
1. **Item 4** (reduz ruído de I/O, facilita validar os demais por log).
2. **Item 1** (suspensão de renderização sob fullscreen na barra).
3. **Item 3** (otimização WM independente).

## Validação
- **Item 1:** lançar jogo em fullscreen; confirmar que a barra **some** sob fullscreen e **reaparece imediatamente** ao fechar/minimizar/Alt-Tab.
- **Item 3:** contar `place_top` por segundo em fullscreen estático (deve cair a ~0 sem mudança de layout).
- **Item 4:** confirmar nível default e queda de writes em disco via `strace`/contagem de I/O.

## Invariantes preservados
- Barra volta a renderizar imediatamente ao fechar/minimizar/perder foco do jogo fullscreen.
- Compatibilidade estrita com protocolos River e especificações wlroots.