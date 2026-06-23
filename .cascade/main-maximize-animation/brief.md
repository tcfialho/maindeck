# Brief: Animação de crescimento ao maximizar a janela main

## Roteamento
T1 — Mudança em 2 arquivos (wm-input.c e wm-layout.c), sem risco de regressão em lógica crítica de estado ou auth.

## Objetivo
Corrigir o comportamento de maximização da janela `main` no `maindeck-wm` para que ocorra uma animação de crescimento (usando `GROW_REVEAL` com clip reveal), idêntica ou similar à animação que ocorre ao maximizar a janela `deck`.

## Contexto da base
- Arquivos:
  - `wm-input.c` (manipula atalhos de teclado como `ACTION_MAXIMIZE_TARGET` e `ACTION_TOGGLE_MAXIMIZE`)
  - `wm-layout.c` (manipula ações de layout e menu de contexto como `md_maximize_window` e `md_restore_window`)
- Padrões a seguir: Uso do enum de intents declarativos (`ANIMATION_INTENT_GROW_REVEAL` / `ANIMATION_INTENT_REFLOW_EASE`) gravados per-window in `window->pending_anim`.
- Gate: Compilação local com meson/ninja e teste de build.

## Contratos e descobertas (conclusões de spikes/pesquisa)
- Atualmente, ao maximizar, a ação marca as janelas com `ANIMATION_INTENT_REFLOW_EASE`.
- O intent `reflow_ease` no river compositor não faz clip reveal (`use_clip_reveal = false`).
- Como a janela `main` tem a mesma posição `x` e `y` (ambos 0) antes e depois de maximizada, a animação de posição não faz nada. Como a escala não é alterada (sfx/sfy = 1.0), a janela `main` simplesmente muda de tamanho instantaneamente sem animação de crescimento.
- A janela `deck` muda de posição `x` (da metade direita para 0), de modo que sua animação de posição (`ANIMATION_INTENT_REFLOW_EASE`) faz com que ela deslize da direita para a esquerda, simulando um crescimento/movimento.
- A animação `GROW_REVEAL` (`ANIMATION_INTENT_GROW_REVEAL`) no river compositor aplica `use_clip_reveal = true`, que anima o corte da janela do tamanho antigo para o novo tamanho, simulando um crescimento perfeito sem distorcer o conteúdo da janela.

## Restrições e invariantes
- Não quebrar o comportamento de restauração (restore/unmaximize), que deve continuar refluindo normalmente.
- Manter o estado interno `wm.maximized` correto.

## Critério de aceitação
- Maximizar a janela `main` (seja por atalho ou menu de contexto) deve disparar a animação de clip-reveal (`GROW_REVEAL`) para fazê-la crescer suavemente.
- Maximizar a janela `deck` deve continuar funcionando e também pode se beneficiar do `GROW_REVEAL`.

## Auto-plano (APENAS T1 — você se planeja; ≤15 linhas)
1. Modificar `ACTION_MAXIMIZE_TARGET` em `wm-input.c` para marcar o `target` com `ANIMATION_INTENT_GROW_REVEAL`.
2. Modificar `ACTION_TOGGLE_MAXIMIZE` em `wm-input.c` para marcar o `target` com `ANIMATION_INTENT_GROW_REVEAL` quando estiver maximizando.
3. Modificar `md_maximize_window` em `wm-layout.c` para marcar a janela com `ANIMATION_INTENT_GROW_REVEAL` e chamar `mark_visible_tiled_anim(ANIMATION_INTENT_REFLOW_EASE)`.
4. Modificar `md_restore_window` em `wm-layout.c` para chamar `mark_visible_tiled_anim(ANIMATION_INTENT_REFLOW_EASE)` ao unmaximizar.
