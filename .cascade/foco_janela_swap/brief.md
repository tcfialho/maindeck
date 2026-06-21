# Brief: foco_janela_swap

## Roteamento
T2 — A alteração toca 10 arquivos de protótipos de animação.

## Objetivo
Adicionar a correção de foco de janela (`wm.target_index = wm.target_index === 0 ? 1 : 0;`) ao realizar o swap de janelas (Ctrl+Hold+Tab) na função `actionSwapMainDeck` nos protótipos em que essa linha está ausente.

## Contexto da base
- Arquivos a serem modificados:
  - `docs/prototype-anim-p2.html`
  - `docs/prototype-anim-p3.html`
  - `docs/prototype-anim-p3.1.html`
  - `docs/prototype-anim-p5.html`
  - `docs/prototype-anim-p5.1.html`
  - `docs/prototype-anim-p7.html`
  - `docs/prototype-anim-p8.html`
  - `docs/prototype-anim-p9.html`
  - `docs/prototype-anim-p10.html`
  - `docs/prototype-anim-p11.html`
- Gate: Conferência manual e validação estática de que a linha do toggle foi adicionada na posição correta em relação ao delay e escopo da animação de cada protótipo.

## Contratos e descobertas
- Em todos estes arquivos, a função `actionSwapMainDeck` faz a troca de `wm.windows[0]` e `wm.windows[1]`, mas não atualiza `wm.target_index` para acompanhar a janela movida.
- A linha de correção necessária é: `wm.target_index = wm.target_index === 0 ? 1 : 0;`

## Restrições e invariantes
- A atribuição de `wm.target_index` deve acontecer logo após o swap do array `wm.windows`, mas antes de chamar as funções de layout ou animação, respeitando também escopos de callbacks/timers (`setTimeout`).

## Critério de aceitação
- O estado de `wm.target_index` é invertido (de 0 para 1 ou de 1 para 0) sempre que o swap ocorre, sincronizando o foco com a janela que foi movida.

## Aberto
- Solicitar ao judge validação da inserção correta em cada um dos 10 protótipos de animação.
