# Brief: Tornar a Animação de Deck Switch mais pronunciada

## Roteamento
T1 — Modificação de 2 arquivos no compositor `river` (Window.zig e Animation.zig) para aumentar a distância de translação e ajustar o traveling clip.

## Objetivo
Tornar a animação de deck switch (Win+Tab+Tab / Win+→ / Win+←) muito mais visível e pronunciada (estilo protótipo) no compositor `river`, aumentando a distância de translação (`dx`) de um valor fixo de 28px para uma proporção dinâmica (30%) da largura da janela, e ajustando o traveling clip para funcionar adequadamente na entrada e na saída de janelas.

## Contexto da base
- Arquivos:
  - `/home/tcfialho/Documents/poc/references/river/river/Window.zig` (armazena as animações dos estados de visibilidade)
  - `/home/tcfialho/Documents/poc/references/river/river/Animation.zig` (define `armDeckIn`, `armDeckOut` e `advanceOrphans`)
- Gate:
  - Compilação do `river`: `cd /home/tcfialho/Documents/poc/references/river && zig build -Doptimize=ReleaseFast`
  - Execução dos testes e verificação visual.

## Contratos e descobertas (conclusões de spikes/pesquisa)
- A translação fixa de 28px é imperceptível em resoluções modernas de alta densidade.
- Usar uma proporção de 30% (`0.30`) da largura da janela (`window.box.width` ou `old_w`) como `dx` torna o movimento claro e dinâmico.
- O *traveling clip* (`clip_travel`) atualmente é ativado incorretamente para `deck_in_right` (entrada da direita), onde não há sobreposição com a janela principal (`MAIN`). Deve ser restrito apenas a movimentos para a esquerda (`dx < 0`).
- A saída de janelas à esquerda (`slide_deck_out_left`) precisa de suporte a *traveling clip* em `advanceOrphans` para evitar que a janela órfã sobreponha a janela principal (`MAIN`) enquanto desliza. O clip do órfão deve ir de `0` a `clip_travel_x` (crescendo ao invés de encolher).

## Restrições e invariantes
- Não alterar outras animações (como minimize/unminimize ou swap).
- O clipe deve ser limpo/removido ao final da animação.

## Critério de aceitação
- A animação de deck switch é claramente perceptível nas direções corretas.
- O clipe impede sobreposições com a `MAIN` nas transições para a esquerda.

## Auto-plano (APENAS T1 — você se planeja; ≤15 linhas)
1. Modificar `armDeckIn` em `Animation.zig` para que `clip_travel` e `clip_travel_x` sejam ativados somente se `dx < 0`.
2. Modificar `armDeckOut` em `Animation.zig` para que `clip_travel` e `clip_travel_x` sejam configurados se `dx < 0`.
3. Ajustar `advanceOrphans` em `Animation.zig` para aplicar `clip_travel` de forma progressiva (`clip_travel_x * progress`) caso a animação do órfão tenha a flag ativada.
4. Modificar `Window.zig` na seção de ocultação (`slide_deck_out`/`slide_deck_out_left`) para calcular `dx` dinamicamente como 30% de `old_w`.
5. Modificar `Window.zig` na seção de entrada (`deck_in_left`/`deck_in_right`) para calcular `dx` dinamicamente como 30% de `window.box.width`.
